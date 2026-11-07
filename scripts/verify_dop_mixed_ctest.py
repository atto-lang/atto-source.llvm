#!/usr/bin/env python3
"""Fail-closed CTest JUnit verifier for AttoLang mixed .atto native execution.

This verifies a test runner *report*, not GitHub run/job provenance. A passing
local or synthetic report MUST NOT be promoted to source-compiled guest proof.
"""
from __future__ import annotations

import argparse
import math
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

EXPECTED = frozenset({
    "inverted_mixed_source_mir_cpp17",
    "inverted_production_source_mir_cpp17",
    "native_inverted_struct_schema_ast",
    "native_inverted_mixed_source_to_native",
    "native_inverted_production_source_to_native",
})
MIXED_MARKER = (
    "MIXED_ATTO_NATIVE_GUEST_PASS: 8 exit42, 4 trap signals, "
    "10 rejects; IR no-GC screened (not linked/FFI proof)"
)
LEGACY_MARKER = (
    "PASS: production .atto -> typed MIR -> no-GC-screened IR, "
    "10 native exit-42 (O0/O2), 4 bounds signals, 5 source rejects, "
    "5 mode rejects, known-GC denied"
)
REQUIRED_MARKERS = {
    "native_inverted_mixed_source_to_native": MIXED_MARKER,
    "native_inverted_production_source_to_native": LEGACY_MARKER,
}
MAX_XML_BYTES = 1_048_576


def _error(message: str) -> None:
    raise ValueError("DOP_MIXED_CTEST_REJECTED: " + message)


def _nonnegative_integer(value: str | None, label: str) -> int:
    if value is None or re.fullmatch(r"(0|[1-9][0-9]*)", value) is None:
        _error("missing or invalid " + label)
    return int(value)


def _elapsed(value: str | None, label: str) -> float:
    if value is None:
        _error("missing " + label)
    try:
        number = float(value)
    except ValueError:
        _error("invalid " + label)
    if not math.isfinite(number) or not 0 < number <= 3600:
        _error("nonpositive, nonfinite or excessive " + label)
    return number


def verify_junit(raw: bytes) -> dict[str, object]:
    if not raw or len(raw) > MAX_XML_BYTES:
        _error("missing or oversized JUnit report")
    # No document types, external entity definitions or uncontrolled XML
    # processing is needed for the exact CTest report grammar.
    if re.search(rb"<!\s*(?:DOCTYPE|ENTITY)\b", raw, re.IGNORECASE):
        _error("XML DTD/entity declaration prohibited")
    try:
        suite = ET.fromstring(raw)
    except ET.ParseError as exc:
        _error("malformed JUnit XML: " + str(exc))
    if suite.tag != "testsuite":
        _error("expected exactly one CTest testsuite root")
    if _nonnegative_integer(suite.get("tests"), "suite tests") != 5:
        _error("suite tests must equal five")
    for field in ("failures", "errors", "skipped"):
        if _nonnegative_integer(suite.get(field), "suite " + field) != 0:
            _error("suite contains failure/error/skip")
    # Only direct testcase records count. Nested/extra reporting sections are
    # not allowed to hide a duplicated or skipped case.
    cases = list(suite)
    if len(cases) != 5 or any(case.tag != "testcase" for case in cases):
        _error("missing, nested or extra testcase records")
    observed: set[str] = set()
    durations: dict[str, float] = {}
    for case in cases:
        name = case.get("name")
        if name not in EXPECTED or name in observed:
            _error("unexpected or duplicated test case")
        observed.add(name)
        if case.get("status") != "run":
            _error("testcase is not reported as actually run: " + name)
        durations[name] = _elapsed(case.get("time"), "case time " + name)
        children = list(case)
        if any(child.tag not in ("system-out", "system-err") for child in children):
            _error("unexpected test outcome or metadata in " + name)
        if len([child for child in children if child.tag == "system-out"]) > 1:
            _error("duplicate system-out in " + name)
        if len([child for child in children if child.tag == "system-err"]) > 1:
            _error("duplicate system-err in " + name)
        if any(list(child) for child in children):
            _error("nested test output in " + name)
        if any((child.text or "").strip() for child in children
               if child.tag == "system-err"):
            _error("unexpected stderr in " + name)
        required = REQUIRED_MARKERS.get(name)
        if required:
            output = next((child.text or "" for child in children
                           if child.tag == "system-out"), "")
            if output.splitlines().count(required) != 1:
                _error("missing or duplicated exact executed stdout marker in " + name)
        # A marker in an unrelated case or in suite-level metadata is not
        # equivalent to actually executing the named native guest test.
        else:
            output = "\n".join(child.text or "" for child in children)
            if any(marker in output for marker in REQUIRED_MARKERS.values()):
                _error("native guest marker in unrelated testcase")
    if observed != EXPECTED:
        _error("missing canonical CTest cases")
    return {"verified_tests": len(observed), "case_seconds": durations,
            "evidence_class": "JUNIT_REPORT_CHECKED_NOT_RUN_JOB_ATTESTED"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("junit_xml", type=Path)
    arguments = parser.parse_args()
    try:
        result = verify_junit(arguments.junit_xml.read_bytes())
    except (ValueError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print("DOP_MIXED_JUNIT_VERIFIED tests=" + str(result["verified_tests"]) +
          " guest_receipt_requires_independent_run_job_attestation=YES")
    return 0


if __name__ == "__main__":
    sys.exit(main())
