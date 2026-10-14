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
    print("native source input budget enforced")
    return 0


if __name__ == "__main__":
    sys.exit(main())
