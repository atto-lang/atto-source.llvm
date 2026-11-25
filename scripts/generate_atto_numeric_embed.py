#!/usr/bin/env python3
"""Reproducible, fail-closed compiler embed of canonical numeric.atto.

The output is a derived C++ literal; stdlib/numeric.atto is the only guest
source of truth. Neither generation nor checking executes compiled guest code.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

EXPECTED_GIT_SHA = "5ced991f1d49b3c2a60fdd72b9003c0a4d18cd72"
MAX_SOURCE_BYTES = 1024 * 1024
RAW_DELIM = "ATTO_NUMERIC"
OUTPUT = "src/main/cpp/oreslang/frontend/GeneratedNumericAtto.inc"
SOURCE = "stdlib/numeric.atto"


def render(source: bytes) -> bytes:
    if not source or len(source) > MAX_SOURCE_BYTES:
        raise ValueError("numeric.atto is absent/empty or exceeds 1 MiB")
    decoded = source.decode("utf-8", errors="strict")
    actual = hashlib.sha1(b"blob " + str(len(source)).encode("ascii") + b"\0" + source).hexdigest()
    if actual != EXPECTED_GIT_SHA:
        raise ValueError(f"numeric.atto identity mismatch: {actual}")
    if ")" + RAW_DELIM + '"' in decoded:
        raise ValueError("reserved raw-string delimiter collision")
    banner = ("// Generated from stdlib/numeric.atto; do not edit manually.\n"
              "// source Git blob SHA-1: " + EXPECTED_GIT_SHA + "\n")
    return (banner + 'R"' + RAW_DELIM + "(" + decoded + ")" + RAW_DELIM + '"\n').encode("utf-8")


def run(repo: Path, *, check: bool) -> int:
    source = repo / SOURCE
    output = repo / OUTPUT
    if source.is_symlink() or not source.is_file():
        raise ValueError("source must be a regular non-symlink file")
    if output.is_symlink():
        raise ValueError("generated embed must not be a symlink")
    expected = render(source.read_bytes())
    if check:
        if not output.is_file() or output.read_bytes() != expected:
            raise ValueError("generated numeric embed differs from canonical .atto")
        print("ATTO_NUMERIC_SOURCE_GENERATION_OK|compiler=NOT_EXECUTED")
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(expected)
        print(f"generated {OUTPUT}")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--check", action="store_true")
    operation.add_argument("--write", action="store_true")
    args = parser.parse_args()
    try:
        sys.exit(run(Path(__file__).resolve().parents[1], check=args.check))
    except (OSError, UnicodeError, ValueError) as exc:
        print("FAIL:", exc, file=sys.stderr)
        sys.exit(1)
