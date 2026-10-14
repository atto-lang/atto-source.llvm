#!/usr/bin/env python3
"""Fail-closed LLVM product AOT / no-tracing-GC source admission.

The actual record/tuple spread subset must remain rejected without any
emitted IR until field ownership, ABI slots and deterministic drops are
implemented. Independently verify *compiled native execution* at -O0/-O2
for currently supported scalar named-call and explicit shared borrow forms.

These checks use explicit exceptions, not Python 'assert': PYTHONOPTIMIZE
must never disable a CI safety gate. This is not certification of general
record lowering, native heaps or full-application zero-GC execution.
"""
import pathlib
import re
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run(args, *, timeout=30):
    return subprocess.run(args, text=True, capture_output=True,
                          check=False, timeout=timeout)


def validate_no_guest_gc(ir, label):
    require(ir.startswith("; ModuleID"), f"{label}: compiler did not emit LLVM IR")
    # Require absence of collector strategy/intrinsics, managed allocation
    # fallback, and native heap allocator calls within compiled guest IR.
    # Stack 'alloca' and llvm.lifetime intrinsics ARE permitted.
    blocked = (
        r"gc\.statepoint", r"gc\.relocate", r"gc\.result",
        r"gc\.safepoint", r"\bgc\s+\"", r"@malloc\b", r"@calloc\b",
        r"@realloc\b", r"@free\b", r"@GC_[a-zA-Z0-9_]*",
        r"@System\.gc\b", r"@_Zn(?:a|w)m?\b",
    )
    for pattern in blocked:
        require(not re.search(pattern, ir),
                f"{label}: collector/allocator symbol in guest IR: {pattern}")


def check_aot(compiler, clang, root, title, program, exit_code):
    source_path = root / (title + ".ores")
    source_path.write_text(program, encoding="utf-8")
    emitted = run([compiler, str(source_path)])
    require(emitted.returncode == 0,
            f"{title}: native source compiler failed: {emitted.stderr}")
    require(not emitted.stderr.strip(),
            f"{title}: unexpected compiler diagnostics: {emitted.stderr}")
    ir = emitted.stdout
    validate_no_guest_gc(ir, title)
    ir_path = root / (title + ".ll")
    ir_path.write_text(ir, encoding="utf-8")
    for optimization in ("0", "2"):
        binary = root / (title + "-O" + optimization)
        compiled = run([clang, "-x", "ir", "-O" + optimization,
                        str(ir_path), "-o", str(binary)])
        require(compiled.returncode == 0,
                f"{title} O{optimization}: native link failed: {compiled.stderr}")
        guest = run([str(binary)])
        require(guest.returncode == exit_code,
                f"{title} O{optimization}: wrong native exit code "
                f"{guest.returncode}, expected {exit_code}: {guest.stderr}")
        require(not guest.stdout and not guest.stderr,
                f"{title} O{optimization}: unexpected guest output")


def check_rejected(compiler, root, name, source, diagnostic):
    path = root / (name.replace(" ", "-") + ".ores")
    path.write_text(source + "\n", encoding="utf-8")
    result = run([compiler, str(path)])
    require(result.returncode != 0,
            f"{name}: unsupported product source was admitted")
    require(not result.stdout.strip(),
            f"{name}: rejected product emitted partial native IR: {result.stdout[:400]}")
    require(diagnostic in result.stderr,
            f"{name}: incorrect reject reason: {result.stderr}")


def main(compiler, clang):
    prefix = "fnc sum(int a, int b): int { return a + b; }\n"
    rejected = {
        "unspread tuple as result": (
            "pub fnc main(): int { return (19, 23); }",
            "native aggregate value requires"),
        "unspread record as result": (
            "pub fnc main(): int { return (a: 19, b: 23); }",
            "native aggregate value requires"),
        "record with borrowed member": (
            "pub fnc main(int x): int { const r = (loan: rt borrow x); return r.loan; }",
            "borrowed reference may only be passed"),
        "record with nested record": (
            "pub fnc main(): int { const r = (child: (a: 19, b: 23)); return 0; }",
            "native aggregate value requires"),
        "record with duplicate label": (
            "pub fnc main(): int { const r = (a: 19, a: 23); return 0; }",
            "duplicate record field"),
        "record with partial take": (
            "pub fnc main(): int { const r = (a: 19, b: 23); return rt take r.a; }",
            "cannot take final class field"),
        "non-Copy call argument in record spread": (
            prefix + "pub fnc main(int x): int { return sum(...(a: rt borrow mut x, b: rt borrow mut x)); }",
            "borrowed reference may only be passed"),
        "unknown dynamic spread": (
            prefix + "pub fnc main(int x): int { return sum(...x); }",
            "native spread requires a statically known closed"),
        "unknown record member": (
            "pub fnc main(): int { const r = (a: 19, b: 23); return r.missing; }",
            "unknown class field or receiver"),
        "unknown spread parameter label": (
            prefix + "pub fnc main(): int { return sum(...(c: 23, a: 19)); }",
            "unknown named argument"),
        "duplicate bound slot": (
            prefix + "pub fnc main(): int { return sum(a: 19, ...(a: 23)); }",
            "duplicate named argument"),
        "missing bound slot": (
            prefix + "pub fnc main(): int { return sum(...(a: 19)); }",
            "wrong call arity"),
        "positional after named spread": (
            prefix + "pub fnc main(): int { return sum(...(a: 19), 23); }",
            "positional argument cannot follow named argument"),
    }
    scalar_named = prefix + """
pub fnc main(): int {
    return sum(b: 23, a: 19);
}
"""
    scalar_record = prefix + """
pub fnc main(): int {
    const r = (b: 23, a: 19);
    return sum(...r);
}
"""
    inline_record = prefix + """
pub fnc main(): int { return sum(...(b: 23, a: 19)); }
"""
    inline_tuple = prefix + """
pub fnc main(): int { return sum(...(19, 23)); }
"""
    scalar_projection = """
pub fnc main(): int {
    const r = (b: 23, a: 19);
    return r.a + r.b;
}
"""
    shared_borrow = """
fnc inspect(borrow int x): int { return x + 1; }
fnc wrapper(int x): int { return inspect(rt borrow x); }
pub fnc main(): int { return wrapper(41); }
"""
    with tempfile.TemporaryDirectory(prefix="oreslang-product-aot-gate-") as tmp:
        root = pathlib.Path(tmp)
        for name, (source, reason) in rejected.items():
            check_rejected(compiler, root, name, source, reason)
        check_aot(compiler, clang, root, "native-scalar-named",
                  scalar_named, 42)
        check_aot(compiler, clang, root, "native-scalar-borrow",
                  shared_borrow, 42)
        check_aot(compiler, clang, root, "native-stored-record-spread",
                  scalar_record, 42)
        check_aot(compiler, clang, root, "native-inline-record-spread",
                  inline_record, 42)
        check_aot(compiler, clang, root, "native-inline-tuple-spread",
                  inline_tuple, 42)
        check_aot(compiler, clang, root, "native-scalar-record-projection",
                  scalar_projection, 42)
    print(f"LLVM product admission: {len(rejected)} no-IR rejects; "
          "six real native guest programs at -O0/-O2, exit 42; "
          "no guest GC or allocator calls in emitted IR")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 3:
            raise ValueError("usage: native_product_strict_aot_gate.py COMPILER CLANG")
        main(sys.argv[1], sys.argv[2])
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as failure:
        print(f"LLVM strict product AOT gate FAILED: {failure}", file=sys.stderr)
        sys.exit(1)
