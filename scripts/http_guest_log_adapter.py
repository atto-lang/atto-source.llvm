#!/usr/bin/env python3
"""Normalize raw executed HTTP guest observations without inventing CI evidence.

This intentionally emits NO run IDs, runner identity, source-tree SHA, or test
counts: the CI producer must independently bind and verify that provenance.
"""
import argparse
import json
import re
import sys
from pathlib import Path

from http_counter_contract import (
    CounterContractError, verify_counter_observations,
)

FIXTURE_BLOB = "a0b3fc658d1460eab62c85b3cc897143ea77a7ff"
GRAAL_PREFIX = "GRAAL_GUEST_SHARED_HANDLER_RESULTS "
LLVM_PREFIX = "LLVM_GUEST_SHARED_HANDLER_PASS "
LLVM_HEADER = re.compile(
    r"LLVM_GUEST_SHARED_HANDLER_PASS cases=([1-9][0-9]*) "
    r"fixture_blob=([0-9a-f]{40})\Z"
)
MAX_LOG_BYTES = 2 * 1024 * 1024


class LogAdapterError(ValueError):
    pass


def no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise LogAdapterError("duplicate JSON key " + repr(key))
        result[key] = value
    return result


def parse_json(fragment):
    try:
        return json.loads(fragment, object_pairs_hook=no_duplicate_keys,
                          parse_constant=lambda value: (_ for _ in ()).throw(
                              LogAdapterError("nonfinite JSON number: " + value)))
    except json.JSONDecodeError as error:
        raise LogAdapterError("invalid observation JSON") from error


def exactly_one(lines, prefix, label):
    selected = [line for line in lines if line.startswith(prefix)]
    if len(selected) != 1:
        raise LogAdapterError(f"{label}: expected exactly one executed output marker")
    return selected[0]


def normalize(backend, raw_log, fixture_blob):
    if backend not in ("graal", "llvm"):
        raise LogAdapterError("unsupported backend")
    if fixture_blob != FIXTURE_BLOB:
        raise LogAdapterError("fixture Git blob does not match canonical source")
    if type(raw_log) is not str or len(raw_log.encode("utf-8")) > MAX_LOG_BYTES:
        raise LogAdapterError("invalid or oversized captured output")
    lines = [line.strip() for line in raw_log.splitlines()]
    if backend == "graal":
        line = exactly_one(lines, GRAAL_PREFIX, "graal")
        observed = parse_json(line[len(GRAAL_PREFIX):])
    else:
        header = exactly_one(lines, LLVM_PREFIX, "llvm")
        match = LLVM_HEADER.fullmatch(header)
        if not match or match.group(2) != FIXTURE_BLOB:
            raise LogAdapterError("llvm: invalid count or fixture identity marker")
        # The native Python harness emits a single raw JSON line on stdout.
        candidates = [line for line in lines if line.startswith('{"backend":')]
        if len(candidates) != 1:
            raise LogAdapterError("llvm: expected exactly one machine JSON line")
        report = parse_json(candidates[0])
        if type(report) is not dict or set(report) != {"backend", "observed"}:
            raise LogAdapterError("llvm: invalid machine report fields")
        if report["backend"] != backend:
            raise LogAdapterError("llvm: backend identity mismatch")
        observed = report["observed"]
        if type(observed) is not dict or int(match.group(1)) != len(observed):
            raise LogAdapterError("llvm: reported execution case count mismatch")
    try:
        verify_counter_observations(observed, require_full_matrix=True)
    except CounterContractError as error:
        raise LogAdapterError(str(error)) from error
    return {"backend": backend, "fixture_blob": fixture_blob, "observed": observed}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=("graal", "llvm"), required=True)
    parser.add_argument("--log", type=Path, required=True,
                        help="raw captured stdout from the executed guest test")
    parser.add_argument("--fixture-blob", default=FIXTURE_BLOB)
    args = parser.parse_args()
    try:
        if args.log.stat().st_size > MAX_LOG_BYTES:
            raise LogAdapterError("oversized captured output")
        record = normalize(args.backend, args.log.read_text(encoding="utf-8"),
                           args.fixture_blob)
    except (LogAdapterError, OSError, UnicodeError) as error:
        print("LOG_ADAPTER_FAIL: " + str(error), file=sys.stderr)
        return 1
    print(json.dumps(record, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
