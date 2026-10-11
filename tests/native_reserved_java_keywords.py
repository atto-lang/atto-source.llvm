#!/usr/bin/env python3
"""Fail-closed Java keyword parity: unsupported syntax must never emit LLVM IR."""
import pathlib
import subprocess
import sys
import tempfile

CASES = {
    "over-function": (
        "pub fnc over(): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "structural-function": (
        "pub fnc structural(): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "over-parameter": (
        "pub fnc main(int over): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "structural-parameter": (
        "pub fnc main(int structural): int { return 42; }",
        "expected non-keyword identifier",
    ),
    "over-expression": (
        "pub fnc main(int value): int { return over; }",
        "expected int expression",
    ),
    "structural-expression": (
        "pub fnc main(int value): int { return structural; }",
        "expected int expression",
    ),
}


def main():
    if len(sys.argv) != 2:
        raise ValueError("expected compiler executable argument")
    with tempfile.TemporaryDirectory(prefix="oreslang-native-keywords-") as tmp:
        for name, (source, diagnostic) in CASES.items():
            fixture = pathlib.Path(tmp) / (name + ".ores")
            fixture.write_text(source + "\n", encoding="utf-8")
            result = subprocess.run(
                [sys.argv[1], str(fixture)],
                capture_output=True,
                text=True,
                check=False,
                timeout=20,
            )
            if (result.returncode == 0 or result.stdout.strip()
                    or diagnostic not in result.stderr):
                raise AssertionError(
                    f"{name}: expected rejection without LLVM IR, "
                    f"returncode={result.returncode}, "
                    f"stdout={result.stdout!r}, stderr={result.stderr!r}"
                )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as exc:
        print(f"native keyword parity contract failed: {exc}", file=sys.stderr)
        sys.exit(1)
