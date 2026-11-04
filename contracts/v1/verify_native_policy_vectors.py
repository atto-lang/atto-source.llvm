#!/usr/bin/env python3
"""Execute the canonical 45 authorization cases against the real C++ policy.

HOST-policy evidence ONLY: this is NOT a compiled .atto guest, native AOT
artifact, or admissible backend-runtime parity report. It tests both authorization and serialized policy admission,
not compiled guest operations.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

PIN = "de3594482d10623241a94cf531fd9e4fe71c49769dd68b2098f4ed5363870ac5"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", type=pathlib.Path, required=True)
    p.add_argument("--vectors", type=pathlib.Path,
                   default=pathlib.Path(__file__).with_name("scoped-permissions-v1.json"))
    args = p.parse_args()
    raw = args.vectors.read_bytes()
    if hashlib.sha256(raw).hexdigest() != PIN:
        raise ValueError("canonical vector SHA-256 mismatch")
    data = json.loads(raw)
    cases = data["cases"]
    auth = [case for case in cases if case["kind"] == "authorize"]
    deserialize = [case for case in cases if case["kind"] == "deserialize"]
    if len(cases) != 52 or len(auth) != 45 or len(deserialize) != 7:
        raise ValueError("expected 45 authorization and 7 serialization vectors")
    if len({case["id"] for case in cases}) != 52:
        raise ValueError("duplicate vector identifier")
    for case in auth:
        cmd = [str(args.binary), case["permission"], case["resource"],
               "1" if case["coarse"] else "0", str(len(case["allow"])),
               *case["allow"], str(len(case["deny"])), *case["deny"]]
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=5, check=False)
        if result.returncode != 0 or result.stdout != case["expected"] + "\n":
            raise AssertionError(
                f"HOST_POLICY_VECTOR_FAILURE id={case['id']!r} "
                f"exit={result.returncode} stdout={result.stdout[:80]!r} "
                f"stderr={result.stderr[:160]!r}")
    for case in deserialize:
        cmd = [str(args.binary), "--deserialize", *case["argv"]]
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=5, check=False)
        if result.returncode != 0 or result.stdout != case["expected"] + "\n":
            raise AssertionError(
                f"HOST_SERIALIZATION_VECTOR_FAILURE id={case['id']!r} "
                f"exit={result.returncode} stdout={result.stdout[:80]!r} "
                f"stderr={result.stderr[:160]!r}")
    print("HOST_SCOPED_POLICY_AUTHORIZATION_PASS 45/45")
    print("HOST_SCOPED_POLICY_SERIALIZATION_PASS 7/7")
    print("GUEST_RUNTIME_PARITY_NOT_VERIFIED")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, AssertionError, subprocess.TimeoutExpired) as error:
        print(f"HOST_SCOPED_POLICY_GATE_FAILED: {error}", file=sys.stderr)
        sys.exit(1)
