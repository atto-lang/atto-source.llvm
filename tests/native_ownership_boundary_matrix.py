#!/usr/bin/env python3
"""Verify native-LLVM fail-closed ownership at all async/channel boundaries."""
import pathlib
import subprocess
import sys
import tempfile

compiler = sys.argv[1]
# Deliberately exercise source forms even though their native lowering does
# not exist. Zero emitted IR is as important as the failing process status.
cases = {
    "await": ("pub fnc main(): int { return await next; }", "native async/channel/select boundary unsupported"),
    "async fnc": ("pub async fnc main(): int { return 42; }", "native actor/async declaration"),
    "actor fnc": ("actor fnc main(): int { return 42; }", "native actor/async declaration"),
    "select": ("pub fnc main(): int { select { default: { return 1; } } return 42; }", "native async/channel/select boundary unsupported"),
    "nb select": ("pub fnc main(): int { nb select { default: { return 1; } } return 42; }", "native async/channel/select boundary unsupported"),
    "do select": ("pub fnc main(): int { do select { default: { return 1; } } return 42; }", "native async/channel/select boundary unsupported"),
    "do nb select": ("pub fnc main(): int { do nb select { default: { return 1; } } return 42; }", "native async/channel/select boundary unsupported"),
    "dynamic select": ("pub fnc main(): int { return select from cases; }", "native async/channel/select boundary unsupported"),
    "nb dynamic select": ("pub fnc main(): int { return nb select from cases; }", "native async/channel/select boundary unsupported"),
    "readch": ("pub fnc main(): int { return readch messages; }", "native async/channel/select boundary unsupported"),
    "nb readch": ("pub fnc main(): int { return nb readch messages; }", "native async/channel/select boundary unsupported"),
    "do readch": ("pub fnc main(): int { do readch messages; return 42; }", "native async/channel/select boundary unsupported"),
    "do nb readch": ("pub fnc main(): int { do nb readch messages; return 42; }", "native async/channel/select boundary unsupported"),
    "writech": ("pub fnc main(): int { writech messages, 1; return 42; }", "native async/channel/select boundary unsupported"),
    "nb writech": ("pub fnc main(): int { nb writech messages, 1; return 42; }", "native async/channel/select boundary unsupported"),
    "do writech": ("pub fnc main(): int { do writech messages, 1; return 42; }", "native async/channel/select boundary unsupported"),
    "do nb writech": ("pub fnc main(): int { do nb writech messages, 1; return 42; }", "native async/channel/select boundary unsupported"),
    "generator yield": ("generator fnc main(): int { yield 42; return 0; }", "native actor/async declaration"),
    "cooperate": ("pub fnc main(): int { return rt cooperate; }", "native rt cooperate requires verified heap-owned continuation lowering"),
    "borrow": ("pub fnc main(int x): int { return rt borrow x; }", "borrowed reference may only be passed"),
    "borrow mut": ("pub fnc main(int x): int { return rt borrow mut x; }", "borrowed reference may only be passed"),
    "take from borrowed parameter": ("fnc view(borrow int x): int { return rt take x; }", "cannot rt take through a borrowed parameter"),
    "borrow of temporary": (
        "fnc view(borrow int x): int { return x; } pub fnc main(): int { return view(rt borrow (40 + 2)); }",
        "native borrow requires an addressable named int parameter"),
    "const local cannot gain exclusive loan": (
        "fnc mutate(borrow mut int x): int { return x; } "
        "pub fnc main(): int { const fixed: int = 42; return mutate(rt borrow mut fixed); }",
        "cannot mutably borrow immutable owner"),
    "immutable/exclusive conflict": (
        "fnc join(borrow int a, borrow mut int b): int { return a + b; } "
        "fnc invoke(int x): int { return join(rt borrow x, rt borrow mut x); }",
        "conflicting active native borrow"),
    "exclusive/exclusive conflict": (
        "fnc join(borrow mut int a, borrow mut int b): int { return a + b; } "
        "fnc invoke(int x): int { return join(rt borrow mut x, rt borrow mut x); }",
        "conflicting active native borrow"),
    "move during shared loan": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, rt take x); }",
        "cannot move int while borrow is active"),
    "read during exclusive loan": (
        "fnc use(borrow mut int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow mut x, x); }",
        "read conflicts with active mutable borrow"),
    "nested call loan conflict": (
        "fnc inner(borrow mut int b): int { return b; } "
        "fnc outer(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return outer(rt borrow x, inner(rt borrow mut x)); }",
        "conflicting active native borrow"),
    "borrow mode mismatch": (
        "fnc use(borrow mut int a): int { return a; } "
        "fnc invoke(int x): int { return use(rt borrow x); }",
        "borrow mode does not match borrowed parameter"),
    "borrow instead of value": (
        "fnc use(int a): int { return a; } "
        "fnc invoke(int x): int { return use(rt borrow x); }",
        "borrowed reference may only be passed"),
    "value instead of borrowed": (
        "fnc use(borrow int a): int { return a; } "
        "fnc invoke(int x): int { return use(x); }",
        "borrowed argument requires explicit rt borrow"),
    "shared cannot strengthen to mutable": (
        "fnc inner(borrow mut int a): int { return a; } "
        "fnc outer(borrow int x): int { return inner(rt borrow mut x); }",
        "cannot mutably reborrow shared parameter"),
    "mutable reborrow double-exclusive": (
        "fnc inner(borrow mut int a, borrow mut int b): int { return a + b; } "
        "fnc outer(borrow mut int x): int { return inner(rt borrow mut x, rt borrow mut x); }",
        "conflicting active native borrow"),
    "mutable reborrow then shared": (
        "fnc inner(borrow mut int a, borrow int b): int { return a + b; } "
        "fnc outer(borrow mut int x): int { return inner(rt borrow mut x, rt borrow x); }",
        "conflicting active native borrow"),
    "shared reborrow then mutable": (
        "fnc inner(borrow int a, borrow mut int b): int { return a + b; } "
        "fnc outer(borrow mut int x): int { return inner(rt borrow x, rt borrow mut x); }",
        "conflicting active native borrow"),
    "read during exclusive reborrow": (
        "fnc inner(borrow mut int a, int b): int { return a + b; } "
        "fnc outer(borrow mut int x): int { return inner(rt borrow mut x, x); }",
        "read conflicts with active mutable borrow"),
    "reborrow then await": (
        "fnc inner(borrow int a, int b): int { return a + b; } "
        "fnc outer(borrow int x): int { return inner(rt borrow x, await next); }",
        "native async/channel/select boundary unsupported"),
    "reborrow then select": (
        "fnc inner(borrow int a, int b): int { return a + b; } "
        "fnc outer(borrow mut int x): int { return inner(rt borrow x, select {}); }",
        "native async/channel/select boundary unsupported"),
    # A live call-argument loan must never cause an unimplemented suspend
    # primitive to be accepted: every one still fails before any LLVM IR.
    "borrow then await": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, await next); }",
        "native async/channel/select boundary unsupported"),
    "borrow then select": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, select {}); }",
        "native async/channel/select boundary unsupported"),
    "borrow then nb select": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, nb select {}); }",
        "native async/channel/select boundary unsupported"),
    "borrow then do nb select": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, do nb select {}); }",
        "native async/channel/select boundary unsupported"),
    "borrow then readch": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, readch channel); }",
        "native async/channel/select boundary unsupported"),
    "borrow then do readch": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, do readch channel); }",
        "native async/channel/select boundary unsupported"),
    "borrow then do nb readch": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, do nb readch channel); }",
        "native async/channel/select boundary unsupported"),
    "borrow then writech": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, writech channel); }",
        "native async/channel/select boundary unsupported"),
    "borrow then do nb writech": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, do nb writech channel); }",
        "native async/channel/select boundary unsupported"),
    "borrow then rt cooperate": (
        "fnc use(borrow int a, int b): int { return a + b; } "
        "fnc invoke(int x): int { return use(rt borrow x, rt cooperate); }",
        "native async/channel/select boundary unsupported"),
    "borrow moved parameter": (
        "fnc use(borrow int a): int { return a; } "
        "fnc invoke(int x): int { return rt take x + use(rt borrow x); }",
        "borrow of explicitly moved int"),
}
with tempfile.TemporaryDirectory(prefix="oreslang-native-borrow-matrix-") as d:
    path = pathlib.Path(d) / "test.ores"
    for name, (source, required) in cases.items():
        path.write_text(source + "\n")
        result = subprocess.run([compiler, str(path)], capture_output=True, text=True,
                                timeout=15, check=False)
        assert result.returncode != 0, (name, "UNSAFE: accepted unsupported source", result.stdout)
        assert not result.stdout.strip(), (name, "UNSAFE: emitted LLVM IR on reject", result.stdout)
        assert required in result.stderr, (name, result.stderr)
print(f"Native LLVM ownership boundary matrix passed: {len(cases)} fail-closed source forms")
