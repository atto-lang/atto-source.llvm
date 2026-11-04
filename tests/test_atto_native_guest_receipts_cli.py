#!/usr/bin/env python3
"""Actual three-argument LLVM guest receipt CLI regression, source-only."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from verify_atto_guest_receipts import SUITES

VERIFIER = Path(__file__).resolve().parent / "verify_atto_guest_receipts.py"


def run(args: list[str], expected_exit: int, label: str) -> None:
    process = subprocess.run(
        [sys.executable, str(VERIFIER), *args],
        capture_output=True, timeout=10, check=False,
    )
    if process.returncode != expected_exit:
        raise RuntimeError(
            f"{label}: expected verifier CLI exit {expected_exit}, "
            f"got {process.returncode}: {process.stderr[:500]!r}"
        )


def main() -> None:
    accepted = 0
    rejected = 0
    with tempfile.TemporaryDirectory(prefix="atto-llvm-cli-") as directory:
        root = Path(directory)
        output = root / "guest.stdout"
        kinds = list(SUITES.keys())
        if set(kinds) != {"pass", "fail", "multi-pass", "multi-fail"}:
            raise RuntimeError("verifier suite inventory changed without CLI test review")

        for kind in kinds:
            payload, code = SUITES[kind]
            output.write_bytes(payload)
            run([str(output), kind, str(code)], 0, kind + "/valid")
            accepted += 1

            run([str(output), kind, str(1 - code)], 1, kind + "/wrong-native-exit")
            rejected += 1
            wrong_kind = next(name for name in kinds if name != kind)
            run([str(output), wrong_kind, str(code)], 1, kind + "/wrong-suite")
            rejected += 1

            output.write_bytes(payload + b"ORES_TEST|PASS|forged/x|\n")
            run([str(output), kind, str(code)], 1, kind + "/forged-extra")
            rejected += 1
            output.write_bytes(payload.replace(b"|SUMMARY|", b"|PASS|", 1))
            run([str(output), kind, str(code)], 1, kind + "/forged-summary")
            rejected += 1
            output.write_bytes(payload)

            run([str(output), kind, "01"], 2, kind + "/noncanonical-exit")
            rejected += 1
            run([str(output), "unknown-suite", str(code)], 2, kind + "/unknown-suite")
            rejected += 1

        output.write_bytes(SUITES["pass"][0])
        link = root / "link.stdout"
        link.symlink_to(output)
        run([str(link), "pass", "0"], 1, "symlink")
        rejected += 1

        fifo = root / "fifo"
        os.mkfifo(fifo)
        run([str(fifo), "pass", "0"], 1, "fifo")
        rejected += 1

        output.write_bytes(b"x" * 4097)
        run([str(output), "pass", "0"], 1, "oversized")
        rejected += 1
        run([str(root / "missing"), "pass", "0"], 1, "missing")
        rejected += 1
        run([str(output), "pass", "0", "extra"], 2, "extra-arg")
        rejected += 1
        run([str(output), "pass", "256"], 2, "out-of-range-exit")
        rejected += 1
    if (accepted, rejected) != (4, 30):
        raise RuntimeError("CLI coverage did not execute the required cases")
    print("PASS LLVM three-argument receipt CLI source-only accepted=4 rejected=30 (NO_GUEST_EXECUTION)")


if __name__ == "__main__":
    main()
