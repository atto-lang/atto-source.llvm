#!/usr/bin/env python3
"""Run the scalar-v1 arithmetic result on the actual LLVM guest backend.

Semantic parity (42) is NOT source->effect IR completeness or RT evidence.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def require(good, msg):
    if not good:
        raise AssertionError(msg)


def execute(args, timeout=30):
    return subprocess.run(args, capture_output=True, stdin=subprocess.DEVNULL,
                          check=False, timeout=timeout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    for binary in (args.compiler, args.clang):
        require(binary.exists() and binary.is_file(), "missing compiler/Clang")
    compiler = args.compiler.resolve()
    clang = args.clang.resolve()
    cases = [
        ("40 + 2", 42),
        ("1 + 2 * 3", 7),
        ("(1 + 2) * (3 + 4)", 21),
        ("40 - 2", 38),
    ]
    with tempfile.TemporaryDirectory(prefix="oreslang-scalar-v1-") as name:
        folder = Path(name)
        source = folder / "scalar.ores"
        ll = folder / "scalar.ll"
        executable = folder / "scalar"
        for expression, expected in cases:
            require(0 <= expected <= 255, "lossy exit-code observation")
            source.write_text(
                "pub fnc main():int{return " + expression + ";}\n",
                encoding="utf-8",
            )
            compiled = execute([str(compiler), str(source)])
            require(compiled.returncode == 0, "native Oreslang compile failed: "
                    + repr(compiled.stderr[:512]))
            require(b"define i32 @main(" in compiled.stdout
                    and b"define i64 @__oreslang_guest_main(" in compiled.stdout,
                    "native compiler omitted main wrapper or guest function")
            ll.write_bytes(compiled.stdout)
            built = execute([str(clang), "-O0", "-x", "ir",
                             str(ll), "-o", str(executable)])
            require(built.returncode == 0, "Clang rejected emitted IR: "
                    + repr(built.stderr[:512]))
            result = execute([str(executable)])
            require(result.returncode == expected
                    and result.stdout == b"" and result.stderr == b"",
                    "scalar result mismatch for " + expression)
        source.write_text(
            "pub fnc main():int{return 9223372036854775808;}\n",
            encoding="utf-8",
        )
        invalid = execute([str(compiler), str(source)])
        require(invalid.returncode != 0 and invalid.stdout == b"",
                "overlarge i64 literal emitted unsafe LLVM IR")
    print("native LLVM scalar guest parity: four actual executable cases passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print("scalar guest parity FAILED: " + str(error), file=sys.stderr)
        sys.exit(1)
