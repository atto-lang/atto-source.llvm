#!/usr/bin/env python3
"""Negative and positive cases for exact-head Actions execution evidence."""
import importlib.util
import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parent
spec = importlib.util.spec_from_file_location("checker", ROOT / "check-actions-execution-evidence.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
SHA = "a" * 40
RUN_ID = 123456
STEP = "Actual regression test"


def fixture():
    run = {"id": RUN_ID, "head_sha": SHA, "run_attempt": 1,
           "status": "completed", "conclusion": "success"}
    job = {"id": 8888, "run_id": RUN_ID, "head_sha": SHA, "run_attempt": 1,
           "status": "completed", "conclusion": "success", "runner_id": 100,
           "runner_name": "GitHub Actions 7", "steps": [
               {"name": "Set up job", "status": "completed", "conclusion": "success"},
               {"name": STEP, "status": "completed", "conclusion": "success"},
               {"name": "Complete job", "status": "completed", "conclusion": "success"}]}
    return run, {"total_count": 1, "jobs": [job]}


def validate(run, jobs, **kwargs):
    return checker.verify(run, jobs, expected_sha=kwargs.get("sha", SHA),
                          expected_run_id=kwargs.get("run_id", RUN_ID),
                          expected_attempt=kwargs.get("attempt", 1),
                          expected_job_count=kwargs.get("expected_jobs", 1),
                          required_steps=kwargs.get("required_steps", [STEP]))


class ExecutionEvidenceTests(unittest.TestCase):
    def test_actual_assigned_runner_and_execution_admits(self):
        run, jobs = fixture()
        self.assertEqual(validate(run, jobs)["verdict"], "executed_success")

    def test_runner_zero_denied(self):
        run, jobs = fixture()
        jobs["jobs"][0]["runner_id"] = 0
        with self.assertRaisesRegex(checker.EvidenceError, "no assigned real runner"):
            validate(run, jobs)

    def test_empty_runner_name_denied(self):
        run, jobs = fixture()
        jobs["jobs"][0]["runner_name"] = ""
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs)

    def test_zero_steps_denied(self):
        run, jobs = fixture()
        jobs["jobs"][0]["steps"] = []
        with self.assertRaisesRegex(checker.EvidenceError, "zero recorded"):
            validate(run, jobs)

    def test_no_required_test_step_denied(self):
        run, jobs = fixture()
        jobs["jobs"][0]["steps"][1]["name"] = "Unrelated step"
        with self.assertRaisesRegex(checker.EvidenceError, "required executed steps absent"):
            validate(run, jobs)

    def test_skipped_or_failing_test_step_denied(self):
        for conclusion in ["skipped", "failure", None]:
            run, jobs = fixture()
            jobs["jobs"][0]["steps"][1]["conclusion"] = conclusion
            with self.assertRaises(checker.EvidenceError):
                validate(run, jobs)

    def test_wrong_head_sha_denied(self):
        run, jobs = fixture()
        jobs["jobs"][0]["head_sha"] = "b" * 40
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs)

    def test_wrong_job_attempt_denied(self):
        run, jobs = fixture()
        jobs["jobs"][0]["run_attempt"] = 2
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs)

    def test_wrong_run_attempt_denied(self):
        run, jobs = fixture()
        run["run_attempt"] = 2
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs)

    def test_run_identity_rejects_boolean_and_float_equivalents(self):
        for key, value in (("id", float(RUN_ID)),
                           ("run_attempt", True),
                           ("run_attempt", 1.0)):
            with self.subTest(key=key, value=value):
                run, jobs = fixture()
                run[key] = value
                with self.assertRaisesRegex(checker.EvidenceError, "identity"):
                    validate(run, jobs)

    def test_job_identity_rejects_boolean_and_float_equivalents(self):
        for key, value in (("run_id", float(RUN_ID)),
                           ("run_attempt", True),
                           ("run_attempt", 1.0)):
            with self.subTest(key=key, value=value):
                run, jobs = fixture()
                jobs["jobs"][0][key] = value
                with self.assertRaisesRegex(checker.EvidenceError, "not pinned"):
                    validate(run, jobs)

    def test_unfinished_or_failed_run_denied(self):
        for val in ["failure", "cancelled", "skipped", None]:
            run, jobs = fixture()
            run["conclusion"] = val
            with self.assertRaises(checker.EvidenceError):
                validate(run, jobs)

    def test_pagination_and_matrix_truncation_denied(self):
        run, jobs = fixture()
        jobs["total_count"] = 2
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs)

    def test_each_matrix_job_must_execute_required_test(self):
        run, jobs = fixture()
        second = copy.deepcopy(jobs["jobs"][0])
        second["id"] = 9999
        second["runner_id"] = 200
        second["runner_name"] = "GitHub Actions 8"
        second["steps"][1]["name"] = "Unrelated setup"
        jobs["jobs"].append(second)
        jobs["total_count"] = 2
        with self.assertRaisesRegex(checker.EvidenceError, "each job"):
            validate(run, jobs, expected_jobs=2)

    def test_complete_matrix_with_distinct_runners_admits(self):
        run, jobs = fixture()
        second = copy.deepcopy(jobs["jobs"][0])
        second["id"] = 9999
        second["runner_id"] = 200
        second["runner_name"] = "GitHub Actions 8"
        jobs["jobs"].append(second)
        jobs["total_count"] = 2
        self.assertEqual(validate(run, jobs, expected_jobs=2)["jobs"], 2)

    def test_duplicate_job_ids_denied(self):
        run, jobs = fixture()
        jobs["jobs"].append(copy.deepcopy(jobs["jobs"][0]))
        jobs["total_count"] = 2
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs, expected_jobs=2)

    def test_duplicate_step_names_denied(self):
        run, jobs = fixture()
        jobs["jobs"][0]["steps"].append(copy.deepcopy(jobs["jobs"][0]["steps"][1]))
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs)

    def test_missing_explicit_required_steps_denied(self):
        run, jobs = fixture()
        with self.assertRaises(checker.EvidenceError):
            validate(run, jobs, required_steps=[])

    def test_setup_or_cleanup_cannot_count_as_required_test(self):
        run, jobs = fixture()
        for name in ["Set up job", "Complete job", "Post Run actions/checkout@v4"]:
            with self.assertRaisesRegex(checker.EvidenceError, "not test execution"):
                validate(run, jobs, required_steps=[name])

    def test_cli_exit_codes_are_fail_closed(self):
        with tempfile.TemporaryDirectory() as d:
            run, jobs = fixture()
            rp, jp = Path(d) / "run.json", Path(d) / "jobs.json"
            rp.write_text(json.dumps(run))
            jp.write_text(json.dumps(jobs))
            cmd = [sys.executable, str(ROOT / "check-actions-execution-evidence.py"),
                   "--run-json", str(rp), "--jobs-json", str(jp),
                   "--sha", SHA, "--run-id", str(RUN_ID), "--attempt", "1",
                   "--expected-jobs", "1", "--required-step", STEP]
            out = subprocess.run(cmd, text=True, capture_output=True)
            self.assertEqual(out.returncode, 0, out.stderr + out.stdout)
            self.assertEqual(json.loads(out.stdout)["verdict"], "executed_success")
            jobs["jobs"][0]["runner_id"] = 0
            jp.write_text(json.dumps(jobs))
            out = subprocess.run(cmd, text=True, capture_output=True)
            self.assertEqual(out.returncode, 1)
            self.assertEqual(json.loads(out.stdout)["verdict"], "unverified")


if __name__ == "__main__":
    unittest.main()
