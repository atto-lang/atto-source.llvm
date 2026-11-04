#!/usr/bin/env python3
"""Exercise the real receipt verifier CLI entrypoint, not just verify().

This is source-only evidence, never an LLVM compiler-backed guest pass.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

from verify_atto_guest_receipts import expected

VERIFIER = Path(__file__).resolve().parent / "verify_atto_guest_receipts.py"


def invoke(*args: str) -> int:
    result = subprocess.run(
        [sys.executable, str(VERIFIER), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )
    return result.returncode


def main() -> None:
    accepted = 0
    rejected = 0
    with tempfile.TemporaryDirectory(prefix="atto-llvm-receipts-cli-") as folder:
        output = Path(folder) / "native-guest.stdout"
        for kind in ("pass", "fail", "multi-pass", "multi-mixed"):
            payload = expected(kind)
            output.write_bytes(payload)
            assert invoke(str(output), kind) == 0, f"valid CLI rejected {kind}"
            accepted += 1

            # A different accepted suite identity cannot reclassify the stream.
            wrong_kind = "multi-mixed" if kind != "multi-mixed" else "multi-pass"
            assert invoke(str(output), wrong_kind) == 1, "wrong suite admitted"
            rejected += 1

            output.write_bytes(payload + b"ORES_TEST|PASS|forged/x|\n")
            assert invoke(str(output), kind) == 1, "extra forged record admitted"
            rejected += 1

            output.write_bytes(payload.replace(b"|SUMMARY|", b"|PASS|", 1))
            assert invoke(str(output), kind) == 1, "missing summary admitted"
            rejected += 1

            assert invoke(str(output), "unknown") == 2, "unknown suite accepted"
            rejected += 1

        output.write_bytes(expected("pass"))
        symlink = Path(folder) / "linked.stdout"
        symlink.symlink_to(output)
        assert invoke(str(symlink), "pass") == 1, "symlink admitted"
        rejected += 1

        output.write_bytes(b"x" * 4097)
        assert invoke(str(output), "pass") == 1, "oversized stream admitted"
        rejected += 1
        assert invoke(str(output), "pass", "extra") == 2, "extra CLI argument admitted"
        rejected += 1
        assert invoke(str(Path(folder) / "missing.stdout"), "pass") == 1, "missing stream admitted"
        rejected += 1

    assert accepted == 4 and rejected == 20
    print("PASS LLVM guest receipt CLI source-only probes accepted=4 rejected=20 (NO_GUEST_EXECUTION)")


if __name__ == "__main__":
    main()
