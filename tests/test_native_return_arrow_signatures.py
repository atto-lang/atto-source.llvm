#!/usr/bin/env python3
"""Compiler-level regression for canonical '->' return signatures.

Runs the real native compiler and never executes generated guest code.
"""
import argparse
import subprocess
import tempfile
from pathlib import Path


VALID = {
    "int_colon": "pub fnc main(): int { return 42; }\n",
    "int_arrow": "pub fnc main() -> int { return 42; }\n",
    "void_colon": "pub fnc main(): void { return; }\n",
    "void_arrow": "pub fnc main() -> void { return; }\n",
    "forward_call_arrows": (
        "pub fnc main() -> int { return add(40, 2); }\n"
        "fnc add(int a, int b) -> int { return a + b; }\n"
    ),
}

INVALID = {
    "split_arrow": "pub fnc main() - > int { return 42; }\n",
    "wrong_arrow": "pub fnc main() => int { return 42; }\n",
    "unsupported_return_type": "pub fnc main() -> Unknown { return 42; }\n",
    "field_arrow": "define class Counter as val int count -> 1; end\n",
}


def run_one(compiler: Path, root: Path, name: str, source: str, expect_success: bool) -> None:
    path = root / f"{name}.ores"
    path.write_text(source, encoding="utf-8")
    result = subprocess.run(
        [str(compiler), str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=30,
    )
    if (result.returncode == 0) != expect_success:
        raise AssertionError(
            f"{name}: expected success={expect_success}, exit={result.returncode}\n"
            f"stdout={result.stdout[:1000]!r}\n"
            f"stderr={result.stderr[:1000]!r}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="ores-native-return-arrow-") as temp:
        root = Path(temp)
        for name, source in VALID.items():
            run_one(compiler, root, name, source, True)
        for name, source in INVALID.items():
            run_one(compiler, root, name, source, False)
    print(f"native return signature separator tests passed: {len(VALID) + len(INVALID)}")


if __name__ == "__main__":
    main()
