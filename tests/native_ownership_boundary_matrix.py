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
    "cooperate": ("pub fnc main(): int { return rt cooperate; }", "native async/channel/select boundary unsupported"),
    "borrow": ("pub fnc main(int x): int { return rt borrow x; }", "native rt borrow requires lifetime/provenance"),
    "borrow mut": ("pub fnc main(int x): int { return rt borrow mut x; }", "native rt borrow requires lifetime/provenance"),
    "borrowed parameter": ("pub fnc main(borrow int x): int { return x; }", "expected 'int'"),
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
