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
    "positional tuple shorthand": """
        fnc take((int, int, string?) value): int { return 1; }
        fnc main(): int { return take((5, 3)); }
    """,
    "structural positional tuple": """
        fnc take(structural (int, int, string?) value): int { return 1; }
        fnc main(): int { return take(...(5, 3, "foo", "start")); }
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
        # Positive control is mandatory. An unavailable, broken, or always-
        # rejecting compiler must NEVER make negative-only tests pass.
        good = pathlib.Path(root) / "supported-scalar.ores"
        good.write_text("pub fnc main(): int { return 42; }\n", encoding="utf-8")
        control = subprocess.run(
            [str(compiler), str(good)], capture_output=True, text=True,
            cwd=root, timeout=15, check=False)
        if control.returncode != 0 or "define i64 @__oreslang_guest_main(" not in control.stdout:
            raise RuntimeError(
                "native structural test has no real working scalar compiler: "
                + control.stderr[:500])
        if {p.name for p in pathlib.Path(root).iterdir()} != {good.name}:
            raise RuntimeError("positive scalar control unexpectedly published a file")
        good.unlink()

        for name, source in PROGRAMS.items():
            path = pathlib.Path(root) / (name.replace(" ", "-") + ".ores")
            path.write_text(source, encoding="utf-8")
            # Keep compiler outputs in the isolated directory and prove a
            # rejected fixture did not leave a runnable/IR artifact.
            before = {item.name for item in pathlib.Path(root).iterdir()}
            result = subprocess.run(
                [str(compiler), str(path)], capture_output=True, text=True,
                cwd=root, timeout=15, check=False)
            after = {item.name for item in pathlib.Path(root).iterdir()}
            if before != after:
                raise RuntimeError(f"{name}: rejected source created artifact(s): {sorted(after - before)}")
            if result.returncode is None or result.returncode <= 0:
                raise RuntimeError(f"{name}: expected a diagnosed compiler failure; got {result.returncode}")
            if not result.stderr.strip():
                raise RuntimeError(f"{name}: missing rejection diagnostic")
            if result.stdout.strip():
                raise RuntimeError(f"{name}: rejected structural source leaked LLVM IR/output")
    print("LLVM structural bootstrap correctly fails closed (not parity)")

if __name__ == "__main__":
    main()
