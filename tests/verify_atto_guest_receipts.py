#!/usr/bin/env python3
"""Fail-closed, source-only verifier for independently authored LLVM guest smokes.

This checks exact ordered bytes and an independently observed native exit status,
not process provenance. Only the pinned real-compiler guest runner provides
compiler-backed evidence; this script's self-tests are never guest evidence.
"""
import os
import pathlib
import re
import stat
import sys
import tempfile

MAX_RECEIPT_BYTES = 4096
SUITES = {
    "pass": (
        b"ORES_TEST|PASS|llvm-native/arith-add|\n"
        b"ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=0\n", 0),
    "fail": (
        b"ORES_TEST|FAIL|llvm-native/arith-failure|expected=42\n"
        b"ORES_TEST|SUMMARY|total=1|passed=0|failed=1|skipped=0|filtered=0\n", 1),
    "multi-pass": (
        b"ORES_TEST|PASS|llvm-native/multi/add|\n"
        b"ORES_TEST|PASS|llvm-native/multi/subtract|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),
    "multi-fail": (
        b"ORES_TEST|PASS|llvm-native/multi/add|\n"
        b"ORES_TEST|FAIL|llvm-native/multi/subtract|expected=42\n"
        b"ORES_TEST|SUMMARY|total=2|passed=1|failed=1|skipped=0|filtered=0\n", 1),
    "m1-pass": (
        b"ORES_TEST|PASS|llvm-m1/basic/add|\n"
        b"ORES_TEST|PASS|llvm-m1/basic/subtract|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),
    "m1-fail-fast": (
        b"ORES_TEST|PASS|llvm-m1/fast/add|\n"
        b"ORES_TEST|FAIL|llvm-m1/fast/subtract|expected=42\n"
        b"ORES_TEST|SUMMARY|total=2|passed=1|failed=1|skipped=0|filtered=0\n", 1),
    "m1-filter-skip": (
        b"ORES_TEST|SKIP|llvm-m1/select/skip|maintenance\n"
        b"ORES_TEST|PASS|llvm-m1/select/pass|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=1|failed=0|skipped=1|filtered=1\n", 0),
    "m1-skipped": (
        b"ORES_TEST|SKIP|llvm-m1/empty/skip|not-run\n"
        b"ORES_TEST|SUMMARY|total=1|passed=0|failed=0|skipped=1|filtered=0\n", 1),
    "m1-filtered": (
        b"ORES_TEST|SUMMARY|total=0|passed=0|failed=0|skipped=0|filtered=1\n", 1),
    "m1-invariants": (
        b"ORES_TEST|PASS|llvm-m1/contracts/atomic-recovery|\n"
        b"ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=0\n", 0),
    "m1-replay": (
        b"ORES_TEST|PASS|llvm-m1/identity/no-replay|\n"
        b"ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=1\n", 0),
    "m2-replay": (
        b"ORES_TEST|PASS|llvm-m2/identity/replay-denied|\n"
        b"ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=1\n", 0),
    "m2-out-of-order": (
        b"ORES_TEST|PASS|llvm-m2/order/first|\n"
        b"ORES_TEST|PASS|llvm-m2/order/second|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),
    "m2-pass": (
        b"ORES_TEST|PASS|llvm-m2/dispatch/equality|\n"
        b"ORES_TEST|PASS|llvm-m2/dispatch/less|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),
    "m2-fail-fast": (
        b"ORES_TEST|FAIL|llvm-m2/fast/failing|expected=42\n"
        b"ORES_TEST|SUMMARY|total=1|passed=0|failed=1|skipped=0|filtered=0\n", 1),
    "m2-filter-skip": (
        b"ORES_TEST|SKIP|llvm-m2/selection/skipped|not-ready\n"
        b"ORES_TEST|SUMMARY|total=1|passed=0|failed=0|skipped=1|filtered=1\n", 1),
    "m2-invalid-recover": (
        b"ORES_TEST|PASS|llvm-m2/recovery/first|\n"
        b"ORES_TEST|PASS|llvm-m2/recovery/second|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),
    "m2-duplicate-recover": (
        b"ORES_TEST|PASS|llvm-m2/identity/first|\n"
        b"ORES_TEST|PASS|llvm-m2/identity/second|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),
    "m2-once": (
        b"ORES_TEST|PASS|llvm-m2/once/body|\n"
        b"ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=0\n", 0),
    "m1-order-integrity": (
        b"ORES_TEST|PASS|llvm-m1/order/first|\n"
        b"ORES_TEST|SKIP|llvm-m1/order/skipped|test-fixture\n"
        b"ORES_TEST|PASS|llvm-m1/order/last|\n"
        b"ORES_TEST|SUMMARY|total=3|passed=2|failed=0|skipped=1|filtered=1\n", 0),
    "m1-invalid-state": (
        b"ORES_TEST|PASS|llvm-m1/state/invalid-recover|\n"
        b"ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=0\n", 0),
    "m2-order-preflight": (
        b"ORES_TEST|PASS|llvm-m2/order/first|\n"
        b"ORES_TEST|PASS|llvm-m2/order/second|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),
    "bounded-pp": (
        b"ORES_TEST|PASS|llvm-native/bounded/first|\n"
        b"ORES_TEST|PASS|llvm-native/bounded/second|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=2|failed=0|skipped=0|filtered=0\n", 0),

    "bounded-pf": (
        b"ORES_TEST|PASS|llvm-native/bounded/first|\n"
        b"ORES_TEST|FAIL|llvm-native/bounded/second|expected=0\n"
        b"ORES_TEST|SUMMARY|total=2|passed=1|failed=1|skipped=0|filtered=0\n", 1),

    "bounded-fp": (
        b"ORES_TEST|FAIL|llvm-native/bounded/first|expected=0\n"
        b"ORES_TEST|PASS|llvm-native/bounded/second|\n"
        b"ORES_TEST|SUMMARY|total=2|passed=1|failed=1|skipped=0|filtered=0\n", 1),

    "bounded-ff": (
        b"ORES_TEST|FAIL|llvm-native/bounded/first|expected=0\n"
        b"ORES_TEST|FAIL|llvm-native/bounded/second|expected=0\n"
        b"ORES_TEST|SUMMARY|total=2|passed=0|failed=2|skipped=0|filtered=0\n", 1),

}


def expected(kind: str) -> bytes:
    if kind not in SUITES:
        raise ValueError("unknown LLVM native guest suite")
    return SUITES[kind][0]


def verify(payload: bytes, kind: str, exit_code: int) -> bool:
    # Exact ASCII, case identity, order, count, delimiter, final newline, EOF
    # and *observed* native exit status; no partial, skipped or filtered claims.
    return kind in SUITES and exit_code == SUITES[kind][1] and payload == expected(kind)


def read_bounded_regular(path: pathlib.Path) -> bytes:
    # O_NONBLOCK prevents FIFO/special-file hangs; O_NOFOLLOW rejects a leaf
    # symlink. fstat/read use the SAME descriptor to defeat leaf path swaps.
    flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK
    fd = os.open(path, flags)
    with os.fdopen(fd, "rb", buffering=0) as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= MAX_RECEIPT_BYTES:
            raise ValueError("not a nonempty bounded regular receipt file")
        payload = stream.read(MAX_RECEIPT_BYTES + 1)
        after = os.fstat(stream.fileno())
        if (len(payload) != before.st_size or before.st_size != after.st_size
                or before.st_dev != after.st_dev or before.st_ino != after.st_ino):
            raise ValueError("receipt changed or exceeded byte bound during read")
        return payload


def require_rejection(condition: bool, label: str) -> None:
    # Never use Python 'assert': PYTHONOPTIMIZE=1 or =2 disables assertions.
    if condition:
        raise RuntimeError("accepted forged/malformed receipt: " + label)


def self_test() -> None:
    for kind, (valid, code) in SUITES.items():
        if not verify(valid, kind, code):
            raise RuntimeError("valid LLVM guest receipt rejected: " + kind)
        variants = [
            b"", valid[:-1], valid + b"ORES_TEST|PASS|forged/x|\n",
            valid.replace(b"ORES_TEST|", b"LOG ORES_TEST|", 1),
            valid.replace(b"ORES_TEST|", b"\x1b[31mORES_TEST|", 1),
            valid.replace(b"|SUMMARY|", b"|PASS|", 1),
            valid.replace(b"|total=", b"|total=999", 1),
            valid.replace(b"|passed=", b"|passed=9", 1),
            valid.replace(b"|failed=", b"|failed=9", 1),
            b"\n" + valid, valid.replace(b"\n", b"\r\n"),
            valid.replace(b"|", b"\\u007c", 1),
        ]
        # Add an identity mutation only when a case identity exists. An empty or
        # filtered-only summary has no guest case ID to mutate.
        if b"llvm-native/" in valid:
            variants.append(valid.replace(b"llvm-native/", b"llvm-forged/", 1))
        elif b"llvm-m1/" in valid:
            variants.append(valid.replace(b"llvm-m1/", b"llvm-forged/", 1))
        elif b"llvm-m2/" in valid:
            variants.append(valid.replace(b"llvm-m2/", b"llvm-forged/", 1))
        if kind.startswith("bounded-"):
            variants += [
                valid.replace(b"llvm-native/bounded/first", b"llvm-native/bounded/second", 1),
                valid.replace(b"total=2", b"total=1", 1),
                valid.replace(b"passed=", b"passed=9", 1),
            ]
        if kind.startswith("m1-"):
            variants += [
                valid.replace(b"ORES_TEST|SUMMARY|", b"ORES_TEST|PASS|", 1),
                valid.replace(b"|skipped=", b"|skipped=9", 1),
                valid.replace(b"|filtered=", b"|filtered=9", 1),
            ]
        if kind.startswith("m2-"):
            variants += [
                valid.replace(b"ORES_TEST|SUMMARY|", b"ORES_TEST|PASS|", 1),
                valid.replace(b"|skipped=", b"|skipped=9", 1),
                valid.replace(b"|filtered=", b"|filtered=9", 1),
            ]
            if b"|FAIL|" in valid:
                variants.append(valid.replace(b"|FAIL|", b"|PASS|", 1))
            if b"|SKIP|" in valid:
                variants.append(valid.replace(b"|SKIP|", b"|PASS|", 1))
        if kind.startswith("multi-"):
            variants += [
                valid.replace(b"ORES_TEST|PASS|llvm-native/multi/add|\n", b"", 1),
                valid.replace(b"llvm-native/multi/add", b"llvm-native/multi/subtract", 1),
                valid.replace(b"total=2", b"total=1", 1),
            ]
        for index, sample in enumerate(variants):
            if sample == valid:
                raise RuntimeError(f"ineffective mutation: {kind}/{index}")
            require_rejection(verify(sample, kind, code), f"{kind}/mutation-{index}")
        for wrong in (-1, 2, 124, 137, 255, 1 if code == 0 else 0):
            require_rejection(verify(valid, kind, wrong), f"{kind}/exit-{wrong}")
    with tempfile.TemporaryDirectory(prefix="atto-receipt-verify-") as work:
        parent = pathlib.Path(work)
        regular = parent / "out"
        regular.write_bytes(expected("pass"))
        if read_bounded_regular(regular) != expected("pass"):
            raise RuntimeError("valid bounded receipt rejected")
        for label, data in (("empty", b""), ("oversize", b"X" * (MAX_RECEIPT_BYTES + 1))):
            regular.write_bytes(data)
            try:
                read_bounded_regular(regular)
            except (OSError, ValueError):
                pass
            else:
                raise RuntimeError("accepted " + label + " receipt")
        link = parent / "link"
        link.symlink_to(regular)
        fifo = parent / "fifo"
        os.mkfifo(fifo)
        for label, path in (("symlink", link), ("fifo", fifo), ("directory", parent)):
            try:
                read_bounded_regular(path)
            except (OSError, ValueError):
                pass
            else:
                raise RuntimeError("accepted " + label + " receipt")
    print("PASS LLVM receipt verifier mutations (SOURCE_ONLY; NO_GUEST_EXECUTION)")


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        self_test()
        return 0
    if (len(argv) != 3 or argv[1] not in SUITES
            or re.fullmatch(r"(0|[1-9][0-9]{0,2})", argv[2]) is None
            or int(argv[2]) > 255):
        print("usage: verify_atto_guest_receipts.py OUTPUT_FILE SUITE OBSERVED_EXIT", file=sys.stderr)
        return 2
    try:
        payload = read_bounded_regular(pathlib.Path(argv[0]))
        if not verify(payload, argv[1], int(argv[2])):
            raise ValueError("wrong guest receipt stream or observed native exit")
    except (OSError, ValueError) as exc:
        print("invalid LLVM native guest receipts: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
