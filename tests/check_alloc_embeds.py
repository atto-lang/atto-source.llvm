#!/usr/bin/env python3
"""Ensure the slab/deque/actor_heap source candidates are intact.

They are unregistered candidates under stdlib/policy/alloc/ (see
stdlib/policy/README.md); nothing is embedded in the compiler for them.
This is source identity, NOT native guest execution evidence.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNITS = ("slab", "deque", "actor_heap")


def check() -> None:
    registered = (ROOT / "stdlib/LIBRARIES.txt").read_text()
    for unit in UNITS:
        source = ROOT / "stdlib" / "policy" / "alloc" / f"{unit}.atto"
        if source.is_symlink() or not source.is_file():
            raise AssertionError("source must be a regular, non-symlinked file")
        raw = source.read_bytes()
        if not raw or len(raw) > 1_048_576 or b"\0" in raw:
            raise AssertionError("invalid source bytes")
        if f"define module {unit} as" not in raw.decode("utf-8"):
            raise AssertionError(f"{unit} must define module {unit}")
        if f"stdlib/policy/alloc/{unit}.atto" in registered:
            raise AssertionError(f"{unit} must not be registered before it lowers natively")


if __name__ == "__main__":
    check()
    print("ATTO_ALLOC_CANDIDATES_OK")
