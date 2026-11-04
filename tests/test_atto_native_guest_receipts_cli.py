#!/usr/bin/env python3
"""Source-only integration test for the production LLVM guest receipt CLI.

Invoke the real three-argument command line, not verify() in-process.
Passing these probes never counts as compiled AttoLang guest evidence.
"""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from verify_atto_guest_receipts import SUITES

VERIFIER = Path(__file__).resolve().parent / "verify_atto_guest_receipts.py"
REVIEWED = {
    "pass", "fail", "multi-pass", "multi-fail",
    "bounded-pp", "bounded-pf", "bounded-fp", "bounded-ff",
    "m1-pass", "m1-fail-fast", "m1-filter-skip", "m1-skipped", "m1-filtered",
    "m1-invariants", "m1-replay",
    "m2-pass", "m2-fail-fast", "m2-filter-skip", "m2-invalid-recover",
    "m2-duplicate-recover", "m2-once", "m2-replay", "m2-out-of-order",
    "m1-invariants",
}


def invoke(args: list[str], expected_exit: int, label: str) -> None:
    result = subprocess.run(
        [sys.executable, str(VERIFIER), *args],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False, timeout=8,
    )
    if result.returncode != expected_exit:
        raise RuntimeError(
            f"{label}: verifier exit {result.returncode}, "
            f"expected {expected_exit}: {result.stderr[:256]!r}"
        )


def validate_independent_wire_shape(payload: bytes, kind: str, exit_code: int) -> None:
    """Avoid deriving the complete check from the verifier's own oracle."""
    if not payload.endswith(b"\n") or b"\\n" in payload:
        raise RuntimeError("literal backslash-n or missing line feed: " + kind)
    lines = payload.splitlines(keepends=True)
    if not lines or any(not line.endswith(b"\n") for line in lines):
        raise RuntimeError("missing record line-feed: " + kind)
    summaries = [line for line in lines if line.startswith(b"ORES_TEST|SUMMARY|")]
    if len(summaries) != 1 or summaries[0] != lines[-1]:
        raise RuntimeError("summary not final or unique: " + kind)
    row = summaries[0].removesuffix(b"\n").decode("ascii").split("|")[2:]
    fields = {}
    for part in row:
        key, sep, value = part.partition("=")
        if not sep or key in fields or not value.isascii() or not value.isdecimal():
            raise RuntimeError("invalid summary field: " + kind)
        fields[key] = int(value)
    if set(fields) != {"total", "passed", "failed", "skipped", "filtered"}:
        raise RuntimeError("unexpected summary keys: " + kind)
    events = [line.split(b"|", 2)[1] for line in lines[:-1]]
    if any(event not in (b"PASS", b"FAIL", b"SKIP") for event in events):
        raise RuntimeError("unsupported guest event: " + kind)
    if fields["total"] != len(events) or fields["total"] != sum(
        fields[key] for key in ("passed", "failed", "skipped")
    ):
        raise RuntimeError("inconsistent guest summary: " + kind)
    for field, event in (("passed", b"PASS"), ("failed", b"FAIL"), ("skipped", b"SKIP")):
        if fields[field] != events.count(event):
            raise RuntimeError("summary counters disagree with events: " + kind)
    if (exit_code == 0) != (fields["failed"] == 0 and
                            fields["passed"] > 0):
        raise RuntimeError("guest exit does not reflect summary: " + kind)


def main() -> None:
    if set(SUITES) != REVIEWED:
        raise RuntimeError("LLVM receipt suite inventory changed: review CLI fixtures")
    accepted = 0
    rejected = 0
    with tempfile.TemporaryDirectory(prefix="atto-llvm-receipts-cli-") as folder:
        root = Path(folder)
        output = root / "actual-guest.stdout"
        kinds = sorted(SUITES)
        for kind in kinds:
            payload, code = SUITES[kind]
            validate_independent_wire_shape(payload, kind, code)
            output.write_bytes(payload)
            invoke([str(output), kind, str(code)], 0, kind + "/valid")
            accepted += 1

            invoke([str(output), kind, str(1 - code)], 1, kind + "/wrong-exit")
            rejected += 1
            wrong_kind = next(k for k in kinds if k != kind)
            invoke([str(output), wrong_kind, str(code)], 1, kind + "/wrong-suite")
            rejected += 1

            output.write_bytes(payload + b"ORES_TEST|PASS|forged/x|\n")
            invoke([str(output), kind, str(code)], 1, kind + "/extra-record")
            rejected += 1

            output.write_bytes(payload.replace(b"|SUMMARY|", b"|PASS|", 1))
            invoke([str(output), kind, str(code)], 1, kind + "/no-summary")
            rejected += 1

            output.write_bytes(payload)
            invoke([str(output), kind, "01"], 2, kind + "/noncanonical-exit")
            rejected += 1
            invoke([str(output), "forged-suite", str(code)], 2, kind + "/unknown-suite")
            rejected += 1

        output.write_bytes(SUITES["pass"][0])
        link = root / "link.stdout"
        link.symlink_to(output)
        invoke([str(link), "pass", "0"], 1, "symlink")
        rejected += 1

        fifo = root / "fifo.stdout"
        os.mkfifo(fifo)
        invoke([str(fifo), "pass", "0"], 1, "fifo")
        rejected += 1

        output.write_bytes(b"x" * 4097)
        invoke([str(output), "pass", "0"], 1, "oversized")
        rejected += 1
        output.write_bytes(b"")
        invoke([str(output), "pass", "0"], 1, "empty")
        rejected += 1
        invoke([str(root / "missing.stdout"), "pass", "0"], 1, "missing")
        rejected += 1
        invoke([str(root), "pass", "0"], 1, "directory")
        rejected += 1
        invoke([str(output), "pass", "0", "extra"], 2, "bad-arity")
        rejected += 1
        invoke([str(output), "pass", "256"], 2, "invalid-exit")
        rejected += 1

    if accepted != 23 or rejected != 146:
        raise RuntimeError(f"wrong CLI probe counts: {accepted} accepted, {rejected} rejected")
    print(
        "PASS LLVM guest receipt CLI SOURCE_ONLY accepted=23 rejected=146 "
        "(NO_COMPILER_OR_GUEST_EXECUTION)"
    )


if __name__ == "__main__":
    main()
