#!/usr/bin/env python3
"""Fail-closed comparison of independently collected Java/Graal and LLVM results.

This checks record consistency, not authenticity of remote GitHub evidence.
Before generating records, independently verify fetched Git trees and jobs with
the existing exact_source_gate.py. Never manufacture observed outputs.
"""
import argparse
import json
import re
import sys
from pathlib import Path
from http_counter_contract import CounterContractError, verify_counter_observations

SHA = re.compile(r"^[0-9a-f]{40}$")


class ParityError(ValueError):
    pass


def positive_id(value, name):
    if type(value) is not int or value < 1:
        raise ParityError(f"{name} must be a positive integer (not a boolean)")


def nonnegative(value, name):
    if type(value) is not int or value < 0:
        raise ParityError(f"{name} must be a nonnegative integer")


def validate(record, backend, fixture_blob):
    if type(record) is not dict:
        raise ParityError(f"{backend}: missing result object")
    if record.get("backend") != backend:
        raise ParityError(f"{backend}: backend identity mismatch")
    if record.get("fixture_blob") != fixture_blob:
        raise ParityError(f"{backend}: exact fixture Git blob mismatch")
    for field in ("source_tree", "compiler_sha"):
        value = record.get(field)
        if type(value) is not str or not SHA.fullmatch(value):
            raise ParityError(f"{backend}: missing valid {field}")
    for field in ("run_id", "job_id", "runner_id", "executed_tests"):
        positive_id(record.get(field), f"{backend}.{field}")
    for field in ("skipped_tests", "failed_tests", "errored_tests"):
        nonnegative(record.get(field), f"{backend}.{field}")
        if record[field] != 0:
            raise ParityError(f"{backend}: cannot certify skipped/failed/errored cases")
    if record.get("conclusion") != "success":
        raise ParityError(f"{backend}: CI conclusion is not success")
    cases = record.get("observed")
    if type(cases) is not dict or not cases:
        raise ParityError(f"{backend}: no executed observations")
    if record["executed_tests"] < len(cases):
        raise ParityError(f"{backend}: fewer executed tests than observations")
    for case, value in cases.items():
        if type(case) is not str or not case:
            raise ParityError(f"{backend}: invalid case name")
        if type(value) is not int:  # avoid bool being silently equal to 1
            raise ParityError(f"{backend}: non-integer result for {case!r}")
    return cases


def compare(graal, llvm, fixture_blob):
    if type(fixture_blob) is not str or not SHA.fullmatch(fixture_blob):
        raise ParityError("invalid required fixture blob")
    left = validate(graal, "graal", fixture_blob)
    right = validate(llvm, "llvm", fixture_blob)
    try:
        verify_counter_observations(left, require_full_matrix=True)
        verify_counter_observations(right, require_full_matrix=True)
    except CounterContractError as error:
        raise ParityError(str(error)) from error
    if left.keys() != right.keys():
        raise ParityError("backends reported different sets of executed cases")
    mismatches = [key for key in left if left[key] != right[key]]
    if mismatches:
        raise ParityError("observable result mismatch: " + ", ".join(sorted(mismatches)))
    return len(left)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--graal", type=Path, required=True)
    parser.add_argument("--llvm", type=Path, required=True)
    parser.add_argument("--fixture-blob", required=True)
    args = parser.parse_args()
    try:
        graal = json.loads(args.graal.read_text(encoding="utf-8"))
        llvm = json.loads(args.llvm.read_text(encoding="utf-8"))
        cases = compare(graal, llvm, args.fixture_blob)
    except (ParityError, OSError, UnicodeError, ValueError) as error:
        print("PARITY_FAIL: " + str(error), file=sys.stderr)
        return 1
    print(f"PARITY_OBSERVABLE_EQUAL cases={cases} source_blob={args.fixture_blob}")
    print("NOTE: offline record checks require independently verified GitHub run evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
