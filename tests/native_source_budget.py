#!/usr/bin/env python3
"""Fail-closed compiler CLI source-size regression; no positive IR is accepted."""
import pathlib
import subprocess
import sys
import tempfile

LIMIT = 8 * 1024 * 1024


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: native_source_budget.py <oreslang-llvmc>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="oreslang-source-budget-") as tmp:
        oversized = pathlib.Path(tmp) / "too-large.ores"
        oversized.write_bytes(b" " * (LIMIT + 1))
        result = subprocess.run(
            [str(compiler), str(oversized)],
            capture_output=True, text=True, timeout=15, check=False,
        )
        if result.returncode == 0 or result.stdout:
            raise SystemExit("FAIL: oversized input was accepted or emitted IR")
        if "8 MiB admission limit" not in result.stderr:
            raise SystemExit("FAIL: rejection did not come from the input budget")
    # Reject malformed UTF-8 even if the source is well below the byte budget.
    # The patterns cover overlong sequences, UTF-16 surrogates, truncated
    # multibyte scalars, out-of-range Unicode, and binary NUL.
    for label, payload, diagnostic in (
        ("overlong", b"// " + bytes([0xC0, 0xAF]), "not valid UTF-8"),
        ("surrogate", b"// " + bytes([0xED, 0xA0, 0x80]), "not valid UTF-8"),
        ("truncated", b"// " + bytes([0xE2, 0x82]), "not valid UTF-8"),
        ("out-of-range", b"// " + bytes([0xF4, 0x90, 0x80, 0x80]), "not valid UTF-8"),
        ("nul", b"// " + bytes([0x00]), "contains NUL byte"),
    ):
        bad = pathlib.Path(tmp) / (label + ".ores")
        bad.write_bytes(payload)
        rejected = subprocess.run(
            [str(compiler), str(bad)],
            capture_output=True, text=True, timeout=15, check=False,
        )
        if rejected.returncode == 0 or rejected.stdout:
            raise SystemExit(f"FAIL: {label} malformed source accepted or emitted IR")
        if diagnostic not in rejected.stderr:
            raise SystemExit(f"FAIL: {label} bypassed source encoding admission")
    print("native source input budget enforced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
