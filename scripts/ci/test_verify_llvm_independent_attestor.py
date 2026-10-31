#!/usr/bin/env python3
"""Source-free independent attestor negative regressions; never calls GitHub."""
import importlib.util
from pathlib import Path
import unittest

script = Path(__file__).with_name("verify_llvm_independent_attestor.py")
spec = importlib.util.spec_from_file_location("attestor", script)
attestor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(attestor)
repo = attestor.REPOSITORY
sha = "a" * 40
main_sha = "b" * 40


def fixture(suite="cgl-compat", san=True):
    env = {
        "GITHUB_REPOSITORY": repo, "GITHUB_REF": "refs/heads/main",
        "RUNNER_ENVIRONMENT": "self-hosted", "SOURCE_SHA": sha,
        "GITHUB_SHA": main_sha, "PR_NUMBER": "1200",
        "SUITE": suite, "RUN_SANITIZERS": "true" if san else "false",
        "GITHUB_RUN_ID": "100", "GITHUB_RUN_ATTEMPT": "2",
    }
    prefix = "repos/" + repo
    steps = [{"name": name, "status": "completed", "conclusion": "success"}
             for name in attestor.required_steps(suite, san)]
    data = {
        prefix + "/actions/runs/100": {
            "event": "workflow_dispatch", "head_sha": main_sha,
            "head_branch": "main", "path": attestor.WORKFLOW,
        },
        prefix + "/branches/main": {
            "protected": True, "commit": {"sha": main_sha},
        },
        prefix + "/pulls/1200": {
            "head": {"sha": sha, "repo": {"full_name": repo, "private": True}},
            "user": {"id": 1234}, "state": "open",
        },
        prefix + "/pulls/1200/reviews?per_page=100": [
            {"id": 7, "submitted_at": "2026-10-31T17:30:00Z",
             "state": "APPROVED", "commit_id": sha,
             "user": {"login": "ORESoftware", "id": 11139560}},
        ],
        prefix + "/actions/runs/100/attempts/2/jobs?per_page=100": {
            "total_count": 2, "jobs": [
                {"id": 101, "name": "source-exact-validation", "run_attempt": 2,
                 "runner_id": 111, "runner_group_id": 1, "runner_name": "guest-vm",
                 "status": "completed", "conclusion": "success",
                 "labels": ["self-hosted", attestor.EXECUTION_LABEL], "steps": steps},
                {"id": 102, "name": "independent-postrun-attestation", "run_attempt": 2,
                 "runner_id": 112, "runner_group_id": 1, "runner_name": "clean-vm",
                 "status": "in_progress", "conclusion": None,
                 "labels": ["self-hosted", attestor.ATTESTOR_LABEL], "steps": []},
            ],
        },
    }
    return env, data


class AttestorSourceFreeTests(unittest.TestCase):
    def evaluate(self, env, data):
        def get_json(path):
            self.assertIn(path, data)
            return data[path]
        return attestor.verify_live(get_json, env)

    def test_all_suites_and_sanitizer_modes(self):
        for suite in attestor.SUITES:
            for sanitizer in (True, False):
                with self.subTest(suite=suite, sanitizer=sanitizer):
                    self.assertEqual(self.evaluate(*fixture(suite, sanitizer)), (101, 102))

    def test_runner_admission_and_isolation_fail_closed(self):
        for mode in ("zero", "reused", "overlap", "attempt", "empty", "skipped",
                     "wrong_suite_step", "pagination"):
            with self.subTest(mode=mode):
                env, data = fixture()
                r = data["repos/" + repo + "/actions/runs/100/attempts/2/jobs?per_page=100"]
                guest, clean = r["jobs"]
                if mode == "zero":
                    guest["runner_id"] = 0
                elif mode == "reused":
                    clean["runner_id"] = guest["runner_id"]
                elif mode == "overlap":
                    clean["labels"].append(attestor.EXECUTION_LABEL)
                elif mode == "attempt":
                    guest["run_attempt"] = 1
                elif mode == "empty":
                    guest["steps"] = []
                elif mode == "skipped":
                    guest["steps"][0]["conclusion"] = "skipped"
                elif mode == "wrong_suite_step":
                    guest["steps"].pop()
                elif mode == "pagination":
                    r["total_count"] = 100
                with self.assertRaises(attestor.Denied):
                    self.evaluate(env, data)

    def test_metadata_and_independent_review_fail_closed(self):
        for mode in ("main", "moved", "head", "fork", "public", "closed",
                     "review", "author", "event", "ref", "flag"):
            with self.subTest(mode=mode):
                env, data = fixture()
                prefix = "repos/" + repo
                if mode == "main":
                    data[prefix + "/branches/main"]["protected"] = False
                elif mode == "moved":
                    data[prefix + "/branches/main"]["commit"]["sha"] = "c" * 40
                elif mode == "head":
                    data[prefix + "/pulls/1200"]["head"]["sha"] = "c" * 40
                elif mode == "fork":
                    data[prefix + "/pulls/1200"]["head"]["repo"]["full_name"] = "other/repo"
                elif mode == "public":
                    data[prefix + "/pulls/1200"]["head"]["repo"]["private"] = False
                elif mode == "closed":
                    data[prefix + "/pulls/1200"]["state"] = "closed"
                elif mode == "review":
                    data[prefix + "/pulls/1200/reviews?per_page=100"][0]["state"] = "DISMISSED"
                elif mode == "author":
                    data[prefix + "/pulls/1200"]["user"]["id"] = 11139560
                elif mode == "event":
                    data[prefix + "/actions/runs/100"]["event"] = "pull_request"
                elif mode == "ref":
                    env["GITHUB_REF"] = "refs/heads/untrusted"
                elif mode == "flag":
                    env["RUN_SANITIZERS"] = "maybe"
                with self.assertRaises(attestor.Denied):
                    self.evaluate(env, data)

    def test_spoofed_approval_marker_not_used(self):
        env, data = fixture()
        jobs = data["repos/" + repo + "/actions/runs/100/attempts/2/jobs?per_page=100"]["jobs"]
        jobs[0]["steps"][0]["conclusion"] = "skipped"
        jobs[0]["steps"].append({"name": "POST_EXECUTION_EXACT_PR_HEAD_REVIEW=PASS",
                                 "status": "completed", "conclusion": "success"})
        with self.assertRaises(attestor.Denied):
            self.evaluate(env, data)


if __name__ == "__main__":
    unittest.main()
