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
    "python3 tests/atto_lru_llvm_source.py",
    "cmake --build build/atto-lru --parallel 2 --target oreslang-llvmc",
    "ctest --test-dir build/atto-lru -N",
    "atto_lru_hash_quotient atto_lru_probe_wrap",
    "atto_lru_slot_recycle atto_lru_recency_touch",
    "--output-junit build/atto-lru/lru-ctest.xml",
    "set(names)!=required or len(set(names))!=5",
    'c.find("skipped") is not None',
    "ATTO_LLVM_LRU_GUEST_EXECUTION=PASS native_guests=4 source_guards=1",
    "git diff --quiet --exit-code",
    "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
)
FORBIDDEN=(
    "continue-on-error: true",
    "|| true\n          ctest",
    "PRIVATE_SOURCE_MIRROR=PERFORMED",
)

def verify(source: str) -> None:
    counts={\n        "github.event.pull_request.head.sha || github.sha": 3,\n        "python3 tests/atto_lru_llvm_source.py": 2,\n        "git diff --quiet --exit-code": 2,
        'test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"': 2,\n    }\n    if any(source.count(token)!=counts.get(token,1) for token in REQUIRED):
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
            verify(self.source.replace('c.find("skipped") is not None',
                                       'False'))

    def test_unpinned_checkout_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            verify(self.source.replace("actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683",
                                       "actions/checkout@v4"))

    def test_failure_bypass_rejected(self):
        with self.assertRaisesRegex(ValueError, "bypass"):
            verify(self.source + "\ncontinue-on-error: true\n")

if __name__=="__main__":
    unittest.main()
