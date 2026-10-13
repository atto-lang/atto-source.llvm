#!/usr/bin/env python3
"""AOT/no-tracing-GC product admission gate for the LLVM source compiler.

This deliberately tests *both* native success and unsupported product rejection.
Compile failure must not emit partially usable IR or fall back to a GC runtime.
A successful scalar compilation must execute as native machine code at O0 and O2.
This is a restricted-source proof, NOT general heap/actor no-GC certification.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

def run(args, *, timeout=30):
    return subprocess.run(args, text=True, capture_output=True, check=False,
                          timeout=timeout)

def main(compiler, clang):
    prefix = """fnc sum(int a, int b): int { return a + b; }
"""
    rejected = {
        "tuple spread": (prefix + """pub fnc main(): int { return sum(...(19, 23)); }""",
                         "native product spread requires verified"),
        "record spread": (prefix + """pub fnc main(): int { return sum(...(b: 23, a: 19)); }""",
                          "native product spread requires verified"),
        "stored record": ("""pub fnc main(): int { return (a: 19, b: 23); }""",
                          "native record literal requires verified"),
        "record spread with two mutable loans": (
            prefix + """pub fnc main(int x): int {
                return sum(...(a: rt borrow mut x, b: rt borrow mut x));
            }""", "native product spread requires verified"),
        "positional tuple product": ("""pub fnc main(): int { return (19, 23); }""",
                                     "expected ')'"),
    }
    with tempfile.TemporaryDirectory(prefix="oreslang-product-aot-gate-") as temporary:
        root = pathlib.Path(temporary)
        for name, (source, diagnostic) in rejected.items():
            path = root / (name.replace(" ", "-") + ".ores")
            path.write_text(source + "\n", encoding="utf-8")
            result = run([compiler, str(path)])
            assert result.returncode != 0, (name, "unsupported source was admitted")
            assert not result.stdout.strip(), (name, "compiler emitted IR on reject", result.stdout)
            assert diagnostic in result.stderr, (name, "wrong diagnostic", result.stderr)
        # Independently demonstrate the currently supported native named-call
        # ABI is AOT, allocation-free at the guest-IR level, and source-ordered.
        positive = root / "native-scalar-named.ores"
        positive.write_text(prefix + """
pub fnc main(): int {
    return sum(b: 23, a: 19);
}
""", encoding="utf-8")
        emitted = run([compiler, str(positive)])
        assert emitted.returncode == 0, emitted.stderr
        ir = emitted.stdout
        assert ir.startswith("; ModuleID"), ir[:300]
        # Prohibited collector support, GC intrinsics, or dynamic allocations.
        for symbol in (r"gc\.statepoint", r"gc\.relocate", r"gc\.result",
                       r"gc\.safepoint", r"\bgc\s+\"", r"@malloc\s*\(",
                       r"@calloc\s*\(", r"@realloc\s*\(", r"@free\s*\(",
                       r"@GC_[a-zA-Z0-9_]*", r"@System\.gc"):
            assert not re.search(symbol, ir), ("GC/allocator found in IR", symbol)
        ir_path = root / "guest.ll"
        ir_path.write_text(ir, encoding="utf-8")
        for optimization in ("0", "2"):
            binary = root / ("guest-O" + optimization)
            compiled = run([clang, "-x", "ir", "-O" + optimization,
                            str(ir_path), "-o", str(binary)])
            assert compiled.returncode == 0, ("native AOT linking failed", compiled.stderr)
            guest = run([str(binary)])
            assert guest.returncode == 42, (optimization, guest.returncode, guest.stderr)
            assert not guest.stdout, guest.stdout
    print("LLVM product admission: 5 no-IR rejects; native scalar AOT O0/O2 "
          "exit 42; no guest collector/allocator calls")

if __name__ == "__main__":
    try:
        if len(sys.argv) != 3:
            raise ValueError("usage: native_product_strict_aot_gate.py COMPILER CLANG")
        main(sys.argv[1], sys.argv[2])
    except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as failure:
        print(f"LLVM strict product AOT gate FAILED: {failure}", file=sys.stderr)
        sys.exit(1)
