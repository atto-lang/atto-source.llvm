#!/usr/bin/env python3
"""Exercise native compiler size limits at boundaries; never accept IR on rejection."""
import pathlib
import subprocess
import sys
import tempfile

LIMIT = 8 * 1024 * 1024


def invoke(
    compiler: pathlib.Path, path: pathlib.Path, flags: tuple[str, ...] = ()
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(compiler), *flags, str(path)],
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
        malformed.write_bytes(b" " * LIMIT + b"\xff")
        require_rejected(compiler, malformed)

        nul = base / "under-budget-nul.ores"
        nul.write_bytes(b"\x00")
        invalid = invoke(compiler, nul)
        if invalid.returncode == 0 or invalid.stdout or "NUL byte" not in invalid.stderr:
            raise SystemExit("FAIL: under-budget NUL was accepted or misclassified")
        # Under-budget malformed Unicode must fail for encoding, not just syntax.
        for label, payload in (
            ("overlong", bytes([0xC0, 0xAF])),
            ("surrogate", bytes([0xED, 0xA0, 0x80])),
            ("truncated", bytes([0xE2, 0x82])),
            ("out-of-range", bytes([0xF4, 0x90, 0x80, 0x80])),
        ):
            bad = base / (label + ".ores")
            bad.write_bytes(b"// " + payload)
            rejected = invoke(compiler, bad)
            if rejected.returncode == 0 or rejected.stdout:
                raise SystemExit(f"FAIL: {label} was accepted or emitted IR")
            if "not valid UTF-8" not in rejected.stderr:
                raise SystemExit(f"FAIL: {label} bypassed encoding admission")
        # For Cortex-M, enforce its 128 KiB policy *during* streaming, not
        # after the generic host reader has admitted 8 MiB into memory.
        cortex_flags = ("--target=thumbv7m-none-eabi",)
        cortex_limit = 128 * 1024
        cortex_exact = base / "cortex-exact-limit.ores"
        cortex_exact.write_bytes(b" " * cortex_limit)
        exact_result = invoke(compiler, cortex_exact, cortex_flags)
        if "cortex-m source exceeds 128 KiB limit" in exact_result.stderr:
            raise SystemExit("FAIL: exact Cortex-M limit rejected during reading")
        for suffix in (b"x", bytes([0xff])):
            cortex_over = base / ("cortex-over-" + str(suffix[0]) + ".ores")
            cortex_over.write_bytes(b" " * cortex_limit + suffix)
            fail = invoke(compiler, cortex_over, cortex_flags)
            if (fail.returncode == 0 or fail.stdout
                    or "cortex-m source exceeds 128 KiB limit" not in fail.stderr):
                raise SystemExit("FAIL: Cortex-M byte admission did not precede parsing/encoding")

    print("native source input budget: byte boundary, invalid UTF-8, NUL and Cortex-M boundary checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
