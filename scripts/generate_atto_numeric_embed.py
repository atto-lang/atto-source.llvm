#!/usr/bin/env python3
"""Reproducible, fail-closed compiler embed of canonical numeric.atto.

The output is a derived C++ literal; stdlib/numeric.atto is the only guest
source of truth. Neither generation nor checking executes compiled guest code.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import stat
import tempfile
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
    # All directories and source objects remain in the selected repository root.
    # These checks forbid straightforward symlink/hardlink aliasing; they do not
    # claim an untrusted concurrent writer cannot race a checkout.
    for p in (repo / "stdlib", repo / "src", repo / "src/main",
              repo / "src/main/cpp", repo / "src/main/cpp/oreslang",
              repo / "src/main/cpp/oreslang/frontend"):
        if p.is_symlink():
            raise ValueError(f"symlinked source/output directory: {p}")
    if source.is_symlink() or not source.is_file():
        raise ValueError("source must be a regular non-symlink file")
    source_stat = source.lstat()
    if not stat.S_ISREG(source_stat.st_mode) or source_stat.st_nlink != 1:
        raise ValueError("source must have exactly one hardlink")
    if output.is_symlink():
        raise ValueError("generated embed must not be a symlink")
    expected = render(source.read_bytes())
    if check:
        if not output.is_file() or output.lstat().st_nlink != 1 or output.read_bytes() != expected:
            raise ValueError("generated numeric embed differs from canonical .atto")
        print("ATTO_NUMERIC_SOURCE_GENERATION_OK|compiler=NOT_EXECUTED")
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        # Never update the checked-in generated blob via a partial write. The
        # new file is constructed in the destination directory and replaced.
        filename = None
        try:
            with tempfile.NamedTemporaryFile(mode="wb", dir=output.parent,
                                             prefix=".atto-numeric-", delete=False) as fh:
                filename = fh.name
                fh.write(expected)
                fh.flush()
                os.fsync(fh.fileno())
            os.replace(filename, output)
        finally:
            if filename is not None and os.path.exists(filename):
                os.unlink(filename)
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
