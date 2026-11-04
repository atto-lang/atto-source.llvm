#!/usr/bin/env python3
"""Host-only fail-closed tests for the exact-head native AttoLang LRU workflow."""
from pathlib import Path
import unittest

WORKFLOW = (Path(__file__).resolve().parents[1]
            / ".github/workflows/atto-lru-llvm-m0.yml")
REQUIRED = {
    "permissions:\n  contents: read": 1,
    "runs-on: ubuntu-24.04": 1,
    "actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683": 1,
    "persist-credentials: false": 1,
    "github.event.pull_request.head.sha || github.sha": 3,
    'test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"': 2,
    "git diff --quiet --exit-code": 2,
    "python3 tests/atto_lru_llvm_source.py": 2,
    "cmake --build build/atto-lru --parallel 2 --target oreslang-llvmc": 1,
    "ctest --test-dir build/atto-lru -N": 1,
    "atto_lru_hash_quotient atto_lru_probe_wrap": 1,
    "atto_lru_slot_recycle atto_lru_recency_touch": 1,
    "atto_lru_aos_triple_reuse atto_lru_aos_pair_occupancy": 1,
    "atto_lru_aos_padded_zero atto_lru_aosoa_tail_reset": 1,
    "--output-junit build/atto-lru/lru-ctest.xml": 1,
    "len(cases)!=9 or set(names)!=required or len(set(names))!=9": 1,
    'c.find("skipped") is not None': 1,
    "ATTO_LLVM_LRU_GUEST_EXECUTION=PASS native_guests=8 source_guards=1": 1,
    "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02": 1,
}
FORBIDDEN = ("continue-on-error: true", "|| true\n          ctest",
             "PRIVATE_SOURCE_MIRROR=PERFORMED")
def verify(source: str) -> None:
    for token, count in REQUIRED.items():
        if source.count(token) != count:
            raise ValueError("missing/duplicated native workflow gate: " + token)
    if any(token in source for token in FORBIDDEN):
        raise ValueError("unsafe native workflow bypass")
    if "ctest --test-dir build/atto-lru --output-on-failure -V --timeout 120" not in source:
        raise ValueError("native execution command absent")
    if "if: always()" not in source:
        raise ValueError("missing failure artifact capture")

class WorkflowAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = WORKFLOW.read_text(encoding="utf-8")

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

    def test_omitted_aos_pair_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace(
                "atto_lru_aos_triple_reuse atto_lru_aos_pair_occupancy",
                "atto_lru_aos_triple_reuse atto_lru_aos_triple_reuse"))

    def test_omitted_aosoa_tail_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace(
                "atto_lru_aos_padded_zero atto_lru_aosoa_tail_reset",
                "atto_lru_aos_padded_zero atto_lru_aos_padded_zero"))

    def test_incomplete_receipt_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace(
                "native_guests=8 source_guards=1",
                "native_guests=4 source_guards=1"))

    def test_skipped_tests_cannot_claim_pass(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace('c.find("skipped") is not None', 'False'))

    def test_unpinned_checkout_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace(
                "actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683",
                "actions/checkout@v4"))

    def test_failure_bypass_rejected(self):
        with self.assertRaisesRegex(ValueError, "bypass"):
            verify(self.source + "\ncontinue-on-error: true\n")

if __name__ == "__main__":
    unittest.main()
