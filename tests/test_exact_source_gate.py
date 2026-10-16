import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from exact_source_gate import GateError, verify_trees, verify_workflows

A, B, C = "a" * 40, "b" * 40, "c" * 40


def tree(entries, truncated=False):
    return {"truncated": truncated, "tree": [
        {"path": path, "type": "blob", "sha": sha, "mode": mode}
        for path, sha, mode in entries]}


def run(head=C, conclusion="success", steps=None):
    if steps is None:
        steps = [{"name": "mvn verify", "status": "completed", "conclusion": "success"}]
    return {"run": {"id": 101, "run_attempt": 2, "head_sha": head,
                    "status": "completed", "conclusion": conclusion},
            "jobs": [{"id": 201, "run_id": 101, "run_attempt": 2, "head_sha": head,
                      "name": "compiler", "status": "completed",
                      "conclusion": "success", "steps": steps}]}


class ExactSourceGateTest(unittest.TestCase):
    def setUp(self):
        self.src = tree([("src/Actor.java", A, "100644"), ("run.sh", B, "100755")])
        self.dst = tree([("mirror/src/Actor.java", A, "100644"),
                         ("mirror/run.sh", B, "100755")])

    def test_exact_blob_paths_modes(self):
        self.assertEqual(2, verify_trees(self.src, self.dst, "mirror/"))

    def test_changed_mode_rejected(self):
        self.dst["tree"][1]["mode"] = "100644"
        with self.assertRaisesRegex(GateError, "changed"):
            verify_trees(self.src, self.dst, "mirror/")

    def test_changed_blob_rejected(self):
        self.dst["tree"][1]["sha"] = A
        with self.assertRaisesRegex(GateError, "changed"):
            verify_trees(self.src, self.dst, "mirror/")

    def test_missing_file_rejected(self):
        self.dst["tree"].pop()
        with self.assertRaisesRegex(GateError, "missing"):
            verify_trees(self.src, self.dst, "mirror/")

    def test_unexpected_extra_file_rejected(self):
        self.dst["tree"].append({"path": "mirror/extra.txt", "type": "blob",
                                 "sha": A, "mode": "100644"})
        with self.assertRaisesRegex(GateError, "extra"):
            verify_trees(self.src, self.dst, "mirror/")

    def test_truncated_tree_rejected(self):
        self.src["truncated"] = True
        with self.assertRaisesRegex(GateError, "truncated"):
            verify_trees(self.src, self.dst, "mirror/")

    def test_empty_prefix_rejected(self):
        with self.assertRaisesRegex(GateError, "empty"):
            verify_trees(self.src, self.dst, "does-not-exist/")

    def test_successful_executed_job(self):
        self.assertEqual(1, verify_workflows([run()], C, ["compiler"], ["mvn verify"]))

    def test_jobs_from_another_run_attempt_or_head_are_rejected(self):
        for field, wrong in (("run_id", 102), ("run_attempt", 1), ("head_sha", A),
                             ("run_id", True), ("run_attempt", True)):
            with self.subTest(field=field, wrong=wrong):
                evidence = run()
                evidence["jobs"][0][field] = wrong
                with self.assertRaisesRegex(GateError, "does not belong"):
                    verify_workflows([evidence], C, ["compiler"], ["mvn verify"])

    def test_missing_run_identity_fails_closed(self):
        for field in ("id", "run_attempt"):
            evidence = run()
            del evidence["run"][field]
            with self.assertRaisesRegex(GateError, "run ID or attempt"):
                verify_workflows([evidence], C, ["compiler"], ["mvn verify"])

    def test_duplicate_job_cannot_be_reused_across_evidence(self):
        with self.assertRaisesRegex(GateError, "duplicate job"):
            verify_workflows([run(), run()], C, ["compiler"], ["mvn verify"])

    def test_stale_workflow_rejected(self):
        with self.assertRaisesRegex(GateError, "stale"):
            verify_workflows([run(head=B)], C, ["compiler"], ["mvn verify"])

    def test_zero_step_job_rejected(self):
        with self.assertRaisesRegex(GateError, "zero"):
            verify_workflows([run(steps=[])], C, ["compiler"], ["mvn verify"])

    def test_failed_or_skipped_workflow_rejected(self):
        for conclusion in ("failure", "skipped", None):
            with self.subTest(conclusion=conclusion):
                with self.assertRaises(GateError):
                    verify_workflows([run(conclusion=conclusion)], C, ["compiler"], ["mvn verify"])

    def test_required_test_step_cannot_be_skipped(self):
        steps = [{"name": "checkout", "status": "completed", "conclusion": "success"},
                 {"name": "mvn verify", "status": "completed", "conclusion": "skipped"}]
        with self.assertRaisesRegex(GateError, "missing executed"):
            verify_workflows([run(steps=steps)], C, ["compiler"], ["mvn verify"])

    def test_maven_success_does_not_certify_ctest(self):
        with self.assertRaisesRegex(GateError, "missing executed"):
            verify_workflows([run()], C, ["compiler"], ["ctest"])

    def test_release_gate_requires_explicit_jobs_and_steps(self):
        with self.assertRaisesRegex(GateError, "mandatory"):
            verify_workflows([run()], C, [], ["mvn verify"])
        with self.assertRaisesRegex(GateError, "mandatory"):
            verify_workflows([run()], C, ["compiler"], [])


if __name__ == "__main__":
    unittest.main()
