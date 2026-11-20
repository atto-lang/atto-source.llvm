"""Separate current-code provenance: do not rewrite historical LLVM receipts.

The externally executed snapshot predates this test-only follow-up commit.
No CI authorization, source check, merge readiness or C++ sync-task execution
may be inferred from this deliberately scoped record.
"""
import hashlib
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AUDIT = ROOT / "contracts/native-llvm-sync-task-source-audit-v1.json"


def git_blob(raw):
    return hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()


class CurrentSyncTaskSourceAudit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.record = json.loads(AUDIT.read_text(encoding="utf-8"))

    def test_immutable_prior_snapshot_and_current_critical_blob_identity(self):
        r = self.record
        self.assertEqual(r["schema"], "atto.llvm.sync-task-current-source-provenance.v1")
        self.assertEqual(r["source_snapshot"]["commit"],
                         "42b1f0c4223a93e36914cbfc12b2a5c6d06737b4")
        self.assertEqual(r["source_snapshot"]["git_tree"],
                         "65f506a92f6d33eb01402b7b439afaa7473f57a2")
        self.assertEqual(r["source_snapshot"]["git_blob_count"], 217)
        self.assertEqual(len(r["source_snapshot"]["critical_blobs"]), 9)
        for path, sha in r["source_snapshot"]["critical_blobs"].items():
            with self.subTest(path=path):
                self.assertEqual(git_blob((ROOT / path).read_bytes()), sha)

    def test_historical_receipt_files_unchanged(self):
        r = self.record["historical_receipts"]
        self.assertEqual(r["policy"],
                         "IMMUTABLE_OLD_EXECUTION_SNAPSHOTS_NOT_CURRENT_CODE_CLAIMS")
        self.assertEqual(len(r["git_blobs"]), 4)
        for path, sha in r["git_blobs"].items():
            with self.subTest(path=path):
                self.assertEqual(git_blob((ROOT / path).read_bytes()), sha)

    def test_fail_closed_carrier_result_and_missing_governance(self):
        r = self.record
        self.assertEqual(r["status"],
            "EXTERNALLY_EXECUTED_TESTS_WITH_OPEN_GOVERNANCE_AND_RECEIPT_HOLD")
        e = r["external_execution"]
        self.assertEqual(e["overall_conclusion"], "failure")
        self.assertEqual((e["passed_job_count"], e["failed_job_count"]), (7, 1))
        self.assertEqual(e["source_blobs_verified"], 217)
        self.assertTrue(e["original_git_tree_reconstructed"])
        self.assertIn("/actions/runs/44810676918", e["run"])
        g = r["governance"]
        self.assertFalse(g["source_main_protected_at_audit"])
        for key in ("source_export_authorized", "carrier_export_retention_approved",
                    "carrier_is_required_source_check",
                    "review_of_current_source_head_verified"):
            self.assertFalse(g[key], key)
        self.assertEqual(g["source_check_runs_at_audit"], 0)
        self.assertEqual(g["status"], "HOLD_PENDING_INDEPENDENT_ADMIN_AND_SECURITY_REVIEW")


if __name__ == "__main__":
    unittest.main()
