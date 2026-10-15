#!/usr/bin/env python3
"""Native source/LLVM negative admission gate; no guest parity assertion."""
import argparse
import pathlib
import subprocess
import sys


def check(compiler, fixture, should_compile):
    completed = subprocess.run(
        [str(compiler), str(fixture)], check=False, capture_output=True,
        text=True, timeout=25,
    )
    if should_compile:
        if completed.returncode != 0 or "define " not in completed.stdout:
            raise AssertionError(
                f"positive compiler control failed ({fixture.name}): "
                f"exit={completed.returncode}, stderr={completed.stderr!r}"
            )
    elif completed.returncode == 0 or completed.stdout.strip():
        raise AssertionError(
            f"unsupported source admitted or leaked IR ({fixture.name}): "
            f"exit={completed.returncode}, stdout={completed.stdout[:1000]!r}, "
            f"stderr={completed.stderr[:1000]!r}"
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixtures", type=pathlib.Path, required=True)
    args = parser.parse_args()
    check(args.compiler, args.fixtures / "forward-call-main.ores", True)
    for name in ("unsupported-option-composition.ores",
                 "unsupported-result-composition.ores",
                 "unsupported-question-propagation.ores"):
        check(args.compiler, args.fixtures / name, False)
    print("native functional composition: positive control + 3 fail-closed gates")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
