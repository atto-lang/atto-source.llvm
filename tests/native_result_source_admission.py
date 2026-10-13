#!/usr/bin/env python3
"""Source-level native Result no-IR admission gate.

The LLVM frontend recognizes the closed nominal Result<void, ArrayError>
signature but has not implemented tagged construction, must-use and match
lowering. Every variant must fail before emitting even partial LLVM IR.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

NEGATIVE = {
    "param-closed": (
        "fnc consume(Result<void, ArrayError> value): int { return 0; }",
        "native Result<void, ArrayError> requires tagged construction"),
    "return-closed": (
        "fnc make(): Result<void, ArrayError> { return Ok(); }",
        "native Result<void, ArrayError> requires tagged construction"),
    "return-foreign-error": (
        "fnc make(): Result<void, String> { return Ok(); }",
        "requires nominal ArrayError specialization"),
    "return-payload": (
        "fnc make(): Result<int, ArrayError> { return Ok(0); }",
        "native generic Result<T,E> requires source tagged-value lowering"),
    "local-colon": (
        "fnc make(): int { val outcome: Result<void, ArrayError> = Ok(); return 0; }",
        "native Result<void, ArrayError> requires tagged construction"),
    "local-type-first": (
        "fnc make(): int { let Result<void, ArrayError> outcome = Ok(); return 0; }",
        "native Result<void, ArrayError> requires tagged construction"),
    "spoof-result-class": (
        "define class Result as\\n  int code: 1\\nend\\npub fnc main(): int { return 0; }",
        "native builtin Result/Option names cannot be redefined as classes"),
    "spoof-option-class": (
        "define class Option as\\n  int code: 1\\nend\\npub fnc main(): int { return 0; }",
        "native builtin Result/Option names cannot be redefined as classes"),
}

CONTROL = "pub fnc main(): int { return 42; }"


def run(compiler: Path, path: Path):
    return subprocess.run(
        [str(compiler), str(path)], capture_output=True,
        check=False, timeout=30,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    args = parser.parse_args()
    if not args.compiler.is_file():
        raise AssertionError("native compiler does not exist")
    with tempfile.TemporaryDirectory(prefix="ores-result-source-admission-") as folder:
        root = Path(folder)
        control = root / "control.ores"
        control.write_text(CONTROL, encoding="utf-8")
        check = run(args.compiler, control)
        if check.returncode != 0 or not check.stdout.startswith(b"; ModuleID"):
            raise AssertionError(
                f"native control failed: rc={check.returncode}, stderr={check.stderr!r}")
        for label, (source, expected) in NEGATIVE.items():
            path = root / (label + ".ores")
            path.write_text(source, encoding="utf-8")
            result = run(args.compiler, path)
            if (result.returncode != 1 or result.stdout
                    or expected.encode() not in result.stderr):
                raise AssertionError(
                    f"{label}: expected no-IR '{expected}', "
                    f"rc={result.returncode} stdout={result.stdout!r} "
                    f"stderr={result.stderr!r}")
    print("PASS: nominal Result source admission fails closed with zero emitted LLVM IR")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"native Result admission gate FAILED: {error}", file=sys.stderr)
        sys.exit(1)
