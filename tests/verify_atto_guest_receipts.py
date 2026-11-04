#!/usr/bin/env python3
"""Fail-closed verifier for the two independent LLVM native guest smoke suites.

This authenticates *shape*, not provenance. The caller must compile and execute
the pinned .atto guest. Do not use this to forge/pass off host receipts.
"""
import pathlib
import stat
import sys


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


def self_test() -> None:
    for kind, code in (("pass", 0), ("fail", 1), ("mixed", 1)):
        valid = expected(kind)
        assert verify(valid, kind, code)
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
            assert not verify(sample, kind, code), "forged/malformed receipt admitted"
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
                assert not verify(sample, kind, code), "wrong multi-case order/accounting accepted"
        for wrong in (-1, 2, 124, 137, (1 if code == 0 else 0)):
            assert not verify(valid, kind, wrong), "wrong execution status admitted"
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
        mode = path.lstat().st_mode
        if not stat.S_ISREG(mode) or path.is_symlink() or path.stat().st_size > 4096:
            raise ValueError("not a bounded regular output file")
        payload = path.read_bytes()
        # The *actual* exit status must be checked by the shell runner too.
        if not verify(payload, argv[2], 0 if argv[2] == "pass" else 1):
            raise ValueError("wrong native guest receipt stream")
    except (OSError, ValueError) as exc:
        print("invalid LLVM native guest receipts: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
