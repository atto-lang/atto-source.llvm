#!/usr/bin/env python3
"""Verify canonical Oreslang guest HTTP handler bytes for differential tests.

Use the source identity as an admission guard, not a proxy for execution.
Both LLVM and Java/Graal still need to compile and run this exact source.
"""
import hashlib
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parents[1]
FIXTURE = HERE / "fixtures" / "http-guest-counter-handler.ores"
EXPECTED_BLOB = "a0b3fc658d1460eab62c85b3cc897143ea77a7ff"

def git_blob(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\x00" + data).hexdigest()

def main() -> int:
    source = FIXTURE.read_bytes()
    actual = git_blob(source)
    if actual != EXPECTED_BLOB:
        print(f"PARITY_FIXTURE_MISMATCH expected={EXPECTED_BLOB} actual={actual}", file=sys.stderr)
        return 1
    print(f"PARITY_FIXTURE_IDENTICAL sha={actual} expected_llvm_and_graal=true")
    print("NOTE: identity alone does not certify source compilation or runtime behavior")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
