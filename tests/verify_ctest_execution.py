#!/usr/bin/env python3
"""Fail closed if a successful CTest exit hides skipped or unexecuted tests.

A locally generated JUnit report corroborates CTest execution only on its trusted
host. It is not independent CI-run authentication or guest isolation parity.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import subprocess
import xml.etree.ElementTree as ET

MIN_TESTS = 250
REQUIRED_GUEST = "wasm32_source_actor_guest_lifecycle"
MAX_REPORT_BYTES = 20 * 1024 * 1024


class ExecutionFailure(ValueError):
    pass


def verify_execution(manifest: object, report: bytes, *, min_tests: int = MIN_TESTS) -> int:
    if not isinstance(manifest, dict) or manifest.get("kind") != "ctestInfo":
        raise ExecutionFailure("invalid CTest inventory schema")
    tests = manifest.get("tests")
    if not isinstance(tests, list):
        raise ExecutionFailure("missing CTest inventory")
    names = [x.get("name") for x in tests if isinstance(x, dict)]
    if len(names) != len(tests) or any(not isinstance(x, str) or not x for x in names):
        raise ExecutionFailure("malformed CTest inventory names")
    if len(names) < min_tests or len(set(names)) != len(names) or REQUIRED_GUEST not in names:
        raise ExecutionFailure("incomplete or duplicate CTest inventory")
    if not isinstance(report, bytes) or not 0 < len(report) <= MAX_REPORT_BYTES:
        raise ExecutionFailure("missing or oversized CTest JUnit report")
    if b"<!DOCTYPE" in report.upper() or b"<!ENTITY" in report.upper():
        raise ExecutionFailure("XML entity declarations prohibited")
    try:
        xml = ET.fromstring(report)
    except ET.ParseError as exc:
        raise ExecutionFailure("invalid CTest JUnit XML")
    if xml.tag == "testsuite":
        suites = [xml]
    elif xml.tag == "testsuites":
        suites = list(xml)
        if not suites or any(x.tag != "testsuite" for x in suites):
            raise ExecutionFailure("unexpected CTest JUnit suite structure")
    else:
        raise ExecutionFailure("unexpected CTest JUnit root")
    cases = []
    for suite in suites:
        rows = list(suite)
        if any(x.tag not in ("testcase", "properties", "system-out", "system-err") for x in rows):
            raise ExecutionFailure("unexpected element in CTest suite")
        current = [x for x in rows if x.tag == "testcase"]
        if suite.get("tests") is None or not suite.get("tests", "").isdigit() or \
                int(suite.get("tests")) != len(current):
            raise ExecutionFailure("CTest JUnit suite test count mismatch")
        for key in ("failures", "errors", "skipped", "disabled"):
            value = suite.get(key)
            if value is not None and (not value.isdigit() or int(value) != 0):
                raise ExecutionFailure("CTest JUnit suite reports " + key)
        cases.extend(current)
    result_names = []
    for case in cases:
        name = case.get("name")
        if not isinstance(name, str) or not name:
            raise ExecutionFailure("nameless CTest execution record")
        if case.get("status") != "run":
            raise ExecutionFailure("CTest did not run registered case: " + name)
        if any(x.tag in ("skipped", "failure", "error") for x in case):
            raise ExecutionFailure("CTest case skipped or failed: " + name)
        if any(x.tag not in ("system-out", "system-err", "properties") for x in case):
            raise ExecutionFailure("unexpected nested CTest case status: " + name)
        result_names.append(name)
    if len(result_names) != len(names) or len(set(result_names)) != len(result_names) or \
            set(result_names) != set(names):
        raise ExecutionFailure("CTest JUnit results do not match complete registered inventory")
    return len(names)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--junit", type=Path, required=True)
    p.add_argument("--min-tests", type=int, default=MIN_TESTS)
    args = p.parse_args()
    try:
        if not args.build_dir.is_absolute() or not args.junit.is_absolute() or args.min_tests < 1:
            raise ExecutionFailure("absolute build/report paths and positive minimum required")
        if args.junit.is_symlink() or not args.junit.is_file() or \
                args.junit.stat().st_size > MAX_REPORT_BYTES:
            raise ExecutionFailure("unsafe or absent JUnit report")
        proc = subprocess.run(["ctest", "--test-dir", str(args.build_dir),
                               "--show-only=json-v1"], text=True,
                              capture_output=True, timeout=30)
        if proc.returncode != 0:
            raise ExecutionFailure("CTest inventory unavailable")
        count = verify_execution(json.loads(proc.stdout), args.junit.read_bytes(),
                                 min_tests=args.min_tests)
    except (ExecutionFailure, OSError, ValueError, subprocess.TimeoutExpired) as exc:
        print("NOT_EXECUTED: " + str(exc), file=sys.stderr)
        return 1
    print("SOURCE_LOCAL_CTEST_JUNIT_COMPLETE_AND_UNSKIPPED", count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
