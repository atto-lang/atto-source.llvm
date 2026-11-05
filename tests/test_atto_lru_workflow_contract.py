#!/usr/bin/env python3
"""Host-only mutation checks for source-owned LLVM LRU guest workflow."""
from pathlib import Path
import unittest

WORKFLOW=(Path(__file__).resolve().parents[1] /
          ".github/workflows/atto-lru-llvm-m0.yml")
REQUIRED=(
    "permissions:\n  contents: read",
    "runs-on: ubuntu-24.04",
    "actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683",
    "persist-credentials: false",
    "github.event.pull_request.head.sha || github.sha",
    'test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"',
    "      - 'tests/atto_lru_junit_gate.py'",
    "      - 'tests/test_atto_lru_junit_gate.py'",
    "      - 'tests/test_atto_lru_ctest_path.py'",
    "      - 'tests/atto_lru_ctest_admission.py'",
    "      - 'tests/test_atto_lru_ctest_admission.py'",
    "      - 'tests/contract.py'",
    "python3 tests/atto_lru_llvm_source.py",
    "cmake --build build/atto-lru --parallel 2 --target oreslang-llvmc",
    "sha256sum -c build/atto-lru/compiler-toolchain.sha256",
    "ctest --test-dir build/atto-lru -N",
    "ctest --test-dir build/atto-lru --show-only=json-v1",
    "python3 tests/atto_lru_ctest_admission.py",
    "atto_lru_hash_quotient atto_lru_probe_wrap",
    "atto_lru_slot_recycle atto_lru_recency_touch",
    "--output-junit lru-ctest.xml",
    "python3 tests/atto_lru_junit_gate.py build/atto-lru/lru-ctest.xml",
    "python3 -m unittest discover -s tests -p 'test_atto_lru_junit_gate.py' -v",
    "python3 -m unittest discover -s tests -p 'test_atto_lru_ctest_path.py' -v",
    "python3 -m unittest discover -s tests -p 'test_atto_lru_ctest_admission.py' -v",

    "git diff --quiet --exit-code",
    "            build/atto-lru/lru-ctest-json.json",
    "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
)
FORBIDDEN=(
    "continue-on-error: true",
    "|| true\n          ctest",
    "PRIVATE_SOURCE_MIRROR=PERFORMED",
)

def verify(source: str) -> None:
    counts={
        "github.event.pull_request.head.sha || github.sha": 3,
        "python3 tests/atto_lru_llvm_source.py": 2,
        "git diff --quiet --exit-code": 2,
        'test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"': 2,
    }
    if any(source.count(token)!=counts.get(token,1) for token in REQUIRED):
        raise ValueError("missing or duplicated native workflow gate")
    if any(token in source for token in FORBIDDEN):
        raise ValueError("unsafe native workflow bypass")
    if "ctest --test-dir build/atto-lru --output-on-failure -V --timeout 120" not in source:
        raise ValueError("native execution command absent")
    if "if: always()" not in source:
        raise ValueError("missing failure artifact capture")

class WorkflowAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=WORKFLOW.read_text(encoding="utf-8")

    def test_reviewed_contract(self):
        verify(self.source)

    def test_noop_compiler_build_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("--target oreslang-llvmc",
                                       "--target oreslang-synthetic"))

    def test_omitted_native_test_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("atto_lru_slot_recycle atto_lru_recency_touch",
                                       "atto_lru_slot_recycle atto_lru_slot_recycle"))

    def test_skipped_tests_cannot_claim_pass(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("python3 tests/atto_lru_junit_gate.py build/atto-lru/lru-ctest.xml",
                                       "echo 'No JUnit validation'"))

    def test_unpinned_checkout_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683",
                                       "actions/checkout@v4"))

    def test_junit_verifier_changed_requires_new_workflow_run(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("      - 'tests/atto_lru_junit_gate.py'", "      - 'tests/moved.py'"))

    def test_contract_harness_change_requires_new_workflow_run(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("      - 'tests/contract.py'", ""))

    def test_native_toolchain_digest_must_be_rechecked(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("sha256sum -c build/atto-lru/compiler-toolchain.sha256", "echo verified"))

    def test_nested_ctest_junit_path_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("--output-junit lru-ctest.xml",
                                       "--output-junit build/atto-lru/lru-ctest.xml"))

    def test_report_path_proof_required(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("python3 -m unittest discover -s tests -p 'test_atto_lru_ctest_path.py' -v",
                                       "echo bypassed"))

    def test_ctest_json_property_gate_must_run(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace(
                "python3 tests/atto_lru_ctest_admission.py",
                "echo 'CTest properties unverified'"))

    def test_json_inventory_must_be_generated(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace(
                "ctest --test-dir build/atto-lru --show-only=json-v1",
                "echo 'skip CTest inventory'"))

    def test_admission_tests_change_must_trigger_ci(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace(
                "      - 'tests/test_atto_lru_ctest_admission.py'",
                "      - 'tests/other.py'"))

    def test_failure_bypass_rejected(self):
        with self.assertRaisesRegex(ValueError, "bypass"):
            verify(self.source + "\ncontinue-on-error: true\n")

if __name__=="__main__":
    unittest.main()
