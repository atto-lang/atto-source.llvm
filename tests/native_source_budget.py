#!/usr/bin/env python3
"""Exercise native compiler size limits at boundaries; never accept IR on rejection."""
import pathlib
import subprocess
import sys
import tempfile

LIMIT = 8 * 1024 * 1024


def invoke(compiler: pathlib.Path, path: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(compiler), str(path)],
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )


def require_rejected(compiler: pathlib.Path, path: pathlib.Path) -> None:
    result = invoke(compiler, path)
    if result.returncode == 0 or result.stdout:
        raise SystemExit("FAIL: oversized source accepted or emitted LLVM IR: " + path.name)
    if "8 MiB admission limit" not in result.stderr:
        raise SystemExit("FAIL: wrong admission diagnostic: " + path.name
                         + ": " + result.stderr[:300])


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: native_source_budget.py <oreslang-llvmc>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="oreslang-source-budget-") as tmp:
        base = pathlib.Path(tmp)
        # Exercise the exact limit before testing overflow: whitespace can be
        # syntactically rejected, but must not trip the byte-size budget.
        exact = base / "exact-limit.ores"
        exact.write_bytes(b" " * LIMIT)
        boundary = invoke(compiler, exact)
        if "8 MiB admission limit" in boundary.stderr:
            raise SystemExit("FAIL: exact-limit input was incorrectly rejected")

        for offset in (1, 8192):
            oversized = base / ("too-large-" + str(offset) + ".ores")
            oversized.write_bytes(b" " * (LIMIT + offset))
            require_rejected(compiler, oversized)

        # Budget rejection must happen before UTF-8 or language parsing, and
        # an under-budget malformed input must still be rejected separately.
        malformed = base / "oversized-malformed.ores"
        malformed.write_bytes(b" " * LIMIT + b"\\xff")
        require_rejected(compiler, malformed)

        nul = base / "under-budget-nul.ores"
        nul.write_bytes(b"\\x00")
        invalid = invoke(compiler, nul)
        if invalid.returncode == 0 or invalid.stdout or "NUL byte" not in invalid.stderr:
            raise SystemExit("FAIL: under-budget NUL was accepted or misclassified")
    print("native source input budget: exact boundary, overflow and NUL checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
