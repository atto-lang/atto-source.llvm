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
    "explicit tuple parameter": """
        fnc take(tuple (int, int, string?) value): int { return 1; }
        fnc main(): int { return 1; }
    """,
    "parenthesized tuple parameter": """
        fnc take((int, int, string?) value): int { return 1; }
        fnc main(): int { return 1; }
    """,
    "structural match": """
        fnc main(): int {
          match 1 over
            on structural { x: int } value -> { return value.x; }
            on _ -> { return 1; }
          end
        }
    """,
    "native map source": """
        fnc main(): int {
          let mut Map<int,int> values = new Map<int,int>();
          return 1;
        }
    """,
    "native map ABI collision": """
        fnc oreslang_i64_map_put(int region, int key, int value): int {
          return 0;
        }
        fnc main(): int { return 1; }
    """,
}

def directory_snapshot(root):
    """Record names, file types, modes and bytes, not merely filenames.

    An erroring compiler is not allowed to overwrite or replace a source file,
    nor to clobber a previously rejected fixture while leaving names intact.
    """
    observed = {}
    for item in pathlib.Path(root).iterdir():
        if item.is_symlink() or not item.is_file():
            raise RuntimeError("unexpected symlink or nonfile compiler artifact: " + item.name)
        observed[item.name] = (item.lstat().st_mode, item.read_bytes())
    return observed


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
        if set(directory_snapshot(root)) != {good.name}:
            raise RuntimeError("positive scalar control unexpectedly published a file")
        if good.read_text(encoding="utf-8") != "pub fnc main(): int { return 42; }\\n".replace("\\n", "\n"):
            raise RuntimeError("positive scalar control modified its source")
        good.unlink()

        for name, source in PROGRAMS.items():
            path = pathlib.Path(root) / (name.replace(" ", "-") + ".ores")
            path.write_text(source, encoding="utf-8")
            # Keep compiler outputs in the isolated directory and prove a
            # rejected fixture did not leave a runnable/IR artifact.
            before = directory_snapshot(root)
            result = subprocess.run(
                [str(compiler), str(path)], capture_output=True, text=True,
                cwd=root, timeout=15, check=False)
            after = directory_snapshot(root)
            if before != after:
                raise RuntimeError(f"{name}: rejected source modified existing input or left artifacts: "
                                   f"{sorted(set(after) ^ set(before))}")
            if result.returncode is None or result.returncode <= 0:
                raise RuntimeError(f"{name}: expected a diagnosed compiler failure; got {result.returncode}")
            if not result.stderr.strip():
                raise RuntimeError(f"{name}: missing rejection diagnostic")
            if name in ("explicit tuple parameter", "parenthesized tuple parameter"):
                required = ("native positional tuple parameter requires verified "
                            "readonly-slot and optional-tail lowering")
                if required not in result.stderr:
                    raise RuntimeError(f"{name}: rejection lacked tuple-specific semantic diagnostic")
            if name == "native map source":
                required = ("native Map<int,int> source lowering requires compiler-computed "
                            "fixed capacity plus Option<int> lookup and deterministic entry "
                            "iteration lowering")
                if required not in result.stderr:
                    raise RuntimeError(f"{name}: rejection lacked map-specific semantic diagnostic")
            if name == "native map ABI collision":
                required = "reserved native Map<int,int> ABI symbol"
                if required not in result.stderr:
                    raise RuntimeError(f"{name}: rejection lacked reserved-map-ABI diagnostic")
            if result.stdout.strip():
                raise RuntimeError(f"{name}: rejected structural source leaked LLVM IR/output")
    print("LLVM structural bootstrap correctly fails closed (not parity)")

if __name__ == "__main__":
    main()
