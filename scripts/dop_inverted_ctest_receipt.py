#!/usr/bin/env python3
"""Fail-closed CTest/identity receipt for the restricted LLVM inverted guest.

This records source-local execution. It is not independent authentication of
GitHub's run/job API, cross-backend parity, linked binary safety, or guest ABI.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

EXPECTED_TESTS = frozenset({
    "native_inverted_production_source_to_native",
    "native_inverted_struct_schema_ast",
    "inverted_production_source_mir_cpp17",
    "native_no_gc_ir_audit_unit",
})
SOURCE_PATHS = (
    "CMakeLists.txt",
    "src/main/cpp/oreslang/frontend/Parser.cpp",
    "src/main/cpp/oreslang/types/InvertedProductionSourceMir.hpp",
    "src/main/cpp/oreslang/types/InvertedTypedMir.hpp",
    "src/main/cpp/oreslang/types/InvertedMirLlvmI64.hpp",
    "tests/native_inverted_production_guest.sh",
    "contracts/dop-inverted-v1/swap-reuse.atto",
    "contracts/dop-inverted-v1/removed-index-trap.atto",
    "scripts/audit_no_gc_ir.rs",
)


class InvalidEvidence(ValueError):
    pass


def check_ctest_junit(xml_path):
    """Require exactly the four executed and passing real-source CTest gates."""
    root = ET.parse(xml_path).getroot()
    if root.tag == "testsuite":
        suite = root
    elif root.tag == "testsuites" and len(root) == 1 and root[0].tag == "testsuite":
        suite = root[0]
    else:
        raise InvalidEvidence("unrecognized CTest JUnit test suite")
    cases = list(suite.findall("testcase"))
    names = [case.get("name") for case in cases]
    if len(cases) != len(EXPECTED_TESTS) or set(names) != EXPECTED_TESTS:
        raise InvalidEvidence("missing, duplicate or unexpected CTest case")
    # CTest's own --output-junit emits tests/failures/skipped but omits errors
    # when the count is zero; an explicit nonzero errors attribute is denied.
    for name, expected in (("tests", 4), ("failures", 0), ("skipped", 0)):
        raw = suite.get(name)
        if raw is None or not raw.isascii() or not raw.isdecimal() or int(raw) != expected:
            raise InvalidEvidence(f"contradictory or absent CTest {name} count")
    errors = suite.get("errors", "0")
    if not errors.isascii() or not errors.isdecimal() or int(errors) != 0:
        raise InvalidEvidence("CTest reported errors")
    for case in cases:
        if case.get("status", "run") not in ("run", "passed"):
            raise InvalidEvidence("CTest case was not executed")
        if any(case.find(tag) is not None for tag in ("failure", "error", "skipped")):
            raise InvalidEvidence("CTest case failed or was skipped")
        try:
            elapsed = float(case.attrib["time"])
        except (ValueError, KeyError) as exc:
            raise InvalidEvidence("CTest case lacks valid duration") from exc
        if not math.isfinite(elapsed) or elapsed < 0:
            raise InvalidEvidence("CTest case duration invalid")
    return names


def command(root, *args):
    return subprocess.run(args, cwd=root, check=True, capture_output=True,
                          text=True, timeout=20).stdout.strip()


def artifact_digest(path):
    file = Path(path)
    if not file.is_file() or file.is_symlink():
        raise InvalidEvidence("missing or symlinked compiler/auditor artifact")
    h = hashlib.sha256()
    with file.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def make_receipt(root, xml_path, expected_head, expected_repository, build):
    root, build = Path(root).resolve(), Path(build).resolve()
    if len(expected_head) != 40 or any(c not in "0123456789abcdef" for c in expected_head):
        raise InvalidEvidence("expected head is not a full lowercase git SHA")
    if os.getenv("GITHUB_REPOSITORY") != expected_repository:
        raise InvalidEvidence("wrong GitHub repository")
    if os.getenv("GITHUB_EVENT_NAME") not in ("pull_request", "push", "workflow_dispatch"):
        raise InvalidEvidence("unknown workflow event")
    if os.getenv("GITHUB_RUN_ID", "0").isdigit() is False or int(os.getenv("GITHUB_RUN_ID", "0")) <= 0:
        raise InvalidEvidence("missing workflow run ID")
    if os.getenv("GITHUB_RUN_ATTEMPT", "0").isdigit() is False or int(os.getenv("GITHUB_RUN_ATTEMPT", "0")) <= 0:
        raise InvalidEvidence("missing run attempt")
    if not os.getenv("GITHUB_JOB"):
        raise InvalidEvidence("missing job identity")
    head = command(root, "git", "rev-parse", "HEAD")
    if head != expected_head:
        raise InvalidEvidence("source checkout differs from expected PR head")
    tree = command(root, "git", "rev-parse", "HEAD^{tree}")
    source_blobs = {}
    for path in SOURCE_PATHS:
        committed = command(root, "git", "rev-parse", f"HEAD:{path}")
        working = command(root, "git", "hash-object", "--", path)
        if committed != working:
            raise InvalidEvidence(f"source changed since immutable head: {path}")
        source_blobs[path] = committed
    cases = check_ctest_junit(xml_path)
    binaries = {}
    for name in ("oreslang-llvmc", "oreslang-no-gc-ir-audit"):
        binaries[name] = artifact_digest(build / name)
    return {
        "schema": "atto-dop-inverted-native-ctest-v1",
        "classification": "SOURCE_LOCAL_CTEST_PENDING_INDEPENDENT_RUN_JOB_VERIFICATION",
        "repository": expected_repository,
        "head_sha": head,
        "tree_sha": tree,
        "workflow_run_id": int(os.environ["GITHUB_RUN_ID"]),
        "workflow_attempt": int(os.environ["GITHUB_RUN_ATTEMPT"]),
        "job_name": os.environ["GITHUB_JOB"],
        "executed_ctest_cases": sorted(cases),
        "executed_ctest_count": len(cases),
        "source_git_blobs": source_blobs,
        "built_artifact_sha256": binaries,
        "native_guest_scope": "restricted two-i64 local owner, Clang O0/O2, no-GC IR screen",
        "not_proved": ["independent GitHub run/job identity", "linked binary and FFI no-GC proof",
                       "actor or stable row-handle ownership", "five-backend guest parity"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--ctest-xml", type=Path, required=True)
    parser.add_argument("--expected-head", required=True)
    parser.add_argument("--expected-repository", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    receipt = make_receipt(args.repo_root, args.ctest_xml, args.expected_head,
                           args.expected_repository, args.build)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print("ATTO_DOP_INVERTED_CTEST_REPORTED=4;INDEPENDENT_GITHUB_VERIFICATION=REQUIRED")


if __name__ == "__main__":
    main()
