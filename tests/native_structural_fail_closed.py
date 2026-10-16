#!/usr/bin/env python3
"""Regression gate: unsupported structural source must fail closed on native LLVM.

This is explicitly NOT a positive structural parity test. Once the compiler
supports these constructs, replace this with positive typed/native cases.
"""
import pathlib
import subprocess
import sys
import tempfile

PROGRAMS = {
    "structural parameter": """
        define interface Shape
          x: int;
        end
        fnc take(structural Shape value): int { return value.x; }
        fnc main(): int { return 1; }
    """,
    "structural callable": """
        structural fnc main(): int { return 1; }
    """,
    "structural match": """
        fnc main(): int {
          match 1 over
            on structural { x: int } value -> { return value.x; }
            on _ -> { return 1; }
          end
        }
    """,
}

def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: native_structural_fail_closed.py <oreslang-llvmc>")
    compiler = pathlib.Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="ores-structural-fail-closed-") as root:
        for name, source in PROGRAMS.items():
            path = pathlib.Path(root) / (name.replace(" ", "-") + ".ores")
            path.write_text(source, encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(path)], capture_output=True, text=True,
                timeout=15, check=False)
            if result.returncode is None or result.returncode <= 0:
                raise RuntimeError(f"{name}: expected a diagnosed compiler failure; got {result.returncode}")
            if not result.stderr.strip():
                raise RuntimeError(f"{name}: missing rejection diagnostic")
            if result.stdout.strip():
                raise RuntimeError(f"{name}: rejected structural source leaked LLVM IR/output")
    print("LLVM structural bootstrap correctly fails closed (not parity)")

if __name__ == "__main__":
    main()
