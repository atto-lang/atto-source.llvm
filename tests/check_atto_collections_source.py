#!/usr/bin/env python3
"""Source identity only: must not be reported as LLVM compiler or guest parity."""
from pathlib import Path
import hashlib
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "library/ores.core/collections.atto"
EXPECTED_BLOB = "c5d6659dda078736cb170d5c1d59fc04e700d023"
KINDS = ("array", "vector", "list")
OPERATIONS = ("map", "filter", "reduce")

def check() -> None:
    if SOURCE.is_symlink() or not SOURCE.is_file():
        raise ValueError("collections.atto missing or symlinked")
    raw = SOURCE.read_bytes()
    if not raw or len(raw) > 1_048_576:
        raise ValueError("invalid source byte budget")
    source = raw.decode("utf-8", errors="strict")
    if source.startswith("\ufeff") or "\0" in source:
        raise ValueError("BOM or NUL")
    digest = hashlib.sha1(b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw).hexdigest()
    if digest != EXPECTED_BLOB:
        raise ValueError(f"Atto core Git blob drift: {digest}")
    for kind in KINDS:
        for operation in OPERATIONS:
            name = f"pub fnc {operation}_{kind}<"
            if source.count(name) != 1:
                raise ValueError(f"expected exactly one {operation}_{kind}")
    if "import java" in source or "import dotnet" in source or "do java" in source:
        raise ValueError("foreign source escaped portable core")
    print(f"ATTO_COLLECTIONS_SOURCE_ONLY|git_blob={digest}|compiler=NOT_EXECUTED")

if __name__ == "__main__":
    try:
        check()
    except (OSError, ValueError, UnicodeError) as failure:
        print(f"ATTO_COLLECTIONS_SOURCE_REJECTED={failure}", file=sys.stderr)
        raise SystemExit(1)
