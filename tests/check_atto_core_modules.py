#!/usr/bin/env python3
"""Exact source-snapshot gate; SOURCE ONLY, not compiler/guest evidence."""
from pathlib import Path
import hashlib
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
for stem in ("slice", "iter", "cmp"):
    source = ROOT / "core" / (stem + ".atto")
    snapshot = ROOT / "src/main/cpp/oreslang/frontend" / ("Generated" + stem.title() + "Atto.inc")
    if source.is_symlink() or snapshot.is_symlink():
        sys.exit("refuse symlinked compiler-owned core source/snapshot")
    raw = source.read_bytes()
    if not raw or len(raw) > 65536:
        sys.exit(f"{stem}: malformed source byte budget")
    decoded = raw.decode("utf-8", errors="strict")
    text = snapshot.read_text(encoding="utf-8")
    expected_blob = hashlib.sha1(b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw).hexdigest()
    label = stem.upper()
    # Strip only the exact generated two-line comment header and one raw string.
    expected = (
        f"// Compiler-owned .atto source snapshot; do not edit by hand.\n"
        f"// source Git blob SHA-1: {expected_blob}\n"
        f'R"ATTO_{label}({decoded})ATTO_{label}"\n'
    )
    if text != expected:
        sys.exit(f"{stem}: Git-blob/source-to-embedded-snapshot identity mismatch")
    print(f"CORE_{label}_EXACT_SNAPSHOT={expected_blob}|GUEST_EXECUTION=NOT_EXECUTED")
