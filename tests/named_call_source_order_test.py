#!/usr/bin/env python3
"""Verify named-call argument evaluation order in emitted native LLVM IR."""
import pathlib
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        raise AssertionError("usage: named_call_source_order_test.py COMPILER FIXTURE")
    result = subprocess.run(
        [sys.argv[1], str(pathlib.Path(sys.argv[2]))],
        capture_output=True, text=True, check=False, timeout=25,
    )
    assert result.returncode == 0, result.stderr
    assert result.stdout.startswith("; ModuleID"), result.stdout
    # Source spells second: read_second() before first: read_first().
    # A lowering that prematurely canonicalizes expressions to formal order
    # would reverse the calls, even if the final result remained correct.
    second = result.stdout.find("call i64 @read_second(")
    first = result.stdout.find("call i64 @read_first(")
    assert second >= 0 and first >= 0, "missing native argument evaluation calls"
    assert second < first, "named call was evaluated in formal, not source order"
    assert not result.stderr, result.stderr


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"named call IR order contract failed: {error}", file=sys.stderr)
        sys.exit(1)
