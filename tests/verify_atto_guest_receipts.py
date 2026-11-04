#!/usr/bin/env python3
"""Fail-closed verifier for the three LLVM native guest smoke suites.

This authenticates *shape*, not provenance. The caller must compile and execute
the pinned .atto guest. Do not use this to forge/pass off host receipts.
"""
import os
import pathlib
import stat
import sys
import tempfile


def expected(kind: str) -> bytes:
    if kind == "pass":
        return (b"ORES_TEST|PASS|llvm-native/arith-add|\n"
                b"ORES_TEST|SUMMARY|total=1|passed=1|failed=0|skipped=0|filtered=0\n")
    if kind == "fail":
        return (b"ORES_TEST|FAIL|llvm-native/arith-failure|expected=42\n"
                b"ORES_TEST|SUMMARY|total=1|passed=0|failed=1|skipped=0|filtered=0\n")
    if kind == "mixed":
        return (b"ORES_TEST|PASS|llvm-native/mixed-add|\n"
                b"ORES_TEST|FAIL|llvm-native/mixed-boundary|expected=8\n"
                b"ORES_TEST|SUMMARY|total=2|passed=1|failed=1|skipped=0|filtered=0\n")
    raise ValueError("unknown native receipt suite")


def verify(payload: bytes, kind: str, exit_code: int) -> bool:
    # Exact ASCII, order, count, delimiter, end-of-file and summary contract.
    # An empty/filtered/skipped-only/timeout/compiler failure cannot pass.
    return exit_code == (0 if kind == "pass" else 1) and payload == expected(kind)


def require(condition: bool, message: str) -> None:
    # Must not disappear when Python runs with -O or PYTHONOPTIMIZE=1.
    if not condition:
        raise AssertionError(message)


def read_bounded_receipts(path: pathlib.Path) -> bytes:
    # One no-follow file descriptor plus fstat/read closes the symlink swap
    # between lstat and read_bytes. Never buffer more than 4097 input bytes.
    if not hasattr(os, "O_NOFOLLOW"):
        raise ValueError("secure no-follow regular-file open unsupported")
    flags = os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0)
    fd = os.open(path, flags)
    with os.fdopen(fd, "rb") as output:
        mode = os.fstat(output.fileno()).st_mode
        if not stat.S_ISREG(mode):
            raise ValueError("receipt output must be a regular file")
        data = output.read(4097)
        if len(data) > 4096:
            raise ValueError("native receipt stream exceeds 4096 bytes")
        return data


def self_test() -> None:
    for kind, code in (("pass", 0), ("fail", 1), ("mixed", 1)):
        valid = expected(kind)
        require(verify(valid, kind, code), "valid native receipt rejected")
        mutations = [
            b"", valid[:-1], valid + b"ORES_TEST|PASS|forged/x|\n",
            valid.replace(b"ORES_TEST|", b"LOG ORES_TEST|", 1),
            valid.replace(b"ORES_TEST|", b"\x1b[31mORES_TEST|", 1),
            valid.replace(b"|SUMMARY|", b"|PASS|", 1),
            valid.replace(b"|total=1|", b"|total=2|", 1),
            valid.replace(b"|passed=1|", b"|passed=2|", 1),
            valid.replace(b"|failed=1|", b"|failed=0|", 1),
            b"\n" + valid, valid.replace(b"\n", b"\r\n"),
            valid.replace(b"llvm-native/", b"llvm-wrong/", 1),
            valid.replace(b"|", b"\\u007c", 1),
        ]
        for sample in mutations:
            if sample == valid:
                continue
            require(not verify(sample, kind, code), "forged/malformed receipt admitted")
        if kind == "mixed":
            for sample in (
                valid.replace(b"ORES_TEST|PASS|llvm-native/mixed-add|\n", b""),
                valid.replace(b"ORES_TEST|FAIL|llvm-native/mixed-boundary|expected=8\n", b""),
                valid.replace(b"|passed=1|failed=1|", b"|passed=2|failed=0|"),
                valid.replace(b"ORES_TEST|PASS|llvm-native/mixed-add|\n"
                              b"ORES_TEST|FAIL|llvm-native/mixed-boundary|expected=8\n",
                              b"ORES_TEST|FAIL|llvm-native/mixed-boundary|expected=8\n"
                              b"ORES_TEST|PASS|llvm-native/mixed-add|\n"),
            ):
                require(not verify(sample, kind, code), "wrong multi-case order/accounting accepted")
        for wrong in (-1, 2, 124, 137, (1 if code == 0 else 0)):
            require(not verify(valid, kind, wrong), "wrong execution status admitted")
    # Actual bounded file-open controls, not only byte-string comparisons.
    with tempfile.TemporaryDirectory(prefix="atto-llvm-receipts-") as directory:
        folder = pathlib.Path(directory)
        valid = folder / "valid.txt"
        valid.write_bytes(expected("mixed"))
        require(read_bounded_receipts(valid) == expected("mixed"),
                "bounded regular fixture rejected")
        link = folder / "symlink.txt"
        link.symlink_to(valid)
        try:
            read_bounded_receipts(link)
        except (OSError, ValueError):
            pass
        else:
            raise AssertionError("symlinked native guest receipt admitted")
        oversized = folder / "oversized.txt"
        oversized.write_bytes(b"x" * 4097)
        try:
            read_bounded_receipts(oversized)
        except ValueError:
            pass
        else:
            raise AssertionError("oversized native guest receipt admitted")
    print("PASS LLVM guest receipt source-only verifier mutations (NO_GUEST_EXECUTION)")


def main(argv: list[str]) -> int:
    if argv == ["--self-test"]:
        self_test()
        return 0
    if len(argv) != 3 or argv[2] not in ("pass", "fail", "mixed"):
        print("usage: verify_atto_guest_receipts.py OUTPUT_FILE pass|fail|mixed", file=sys.stderr)
        return 2
    path = pathlib.Path(argv[1])
    try:
        payload = read_bounded_receipts(path)
        # The *actual* exit status must be checked by the shell runner too.
        if not verify(payload, argv[2], 0 if argv[2] == "pass" else 1):
            raise ValueError("wrong native guest receipt stream")
    except (OSError, ValueError) as exc:
        print("invalid LLVM native guest receipts: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
