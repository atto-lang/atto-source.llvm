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
        prefix + "/branches/main/protection": {
            "required_pull_request_reviews": {
                "require_code_owner_reviews": True,
                "dismiss_stale_reviews": True,
                "required_approving_review_count": 1,
                "bypass_pull_request_allowances": {
                    "users": [], "teams": [], "apps": [],
                },
            },
            "required_status_checks": {"strict": True, "contexts": ["llvm-security"], "checks": []},
            "enforce_admins": {"enabled": True},
            "allow_force_pushes": {"enabled": False},
            "allow_deletions": {"enabled": False},
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

    def test_independent_protection_is_not_just_a_badge(self):
        prefix = "repos/" + repo
        key = prefix + "/branches/main/protection"
        for mode in ("missing", "weak_owner", "stale", "no_review", "bypass",
                     "admin", "force_push", "delete", "no_checks",
                     "empty_checks", "non_strict_checks", "invalid_checks"):
            with self.subTest(mode=mode):
                env, data = fixture()
                p = data[key]
                if mode == "missing":
                    del data[key]
                    # Missing protection must fail closed even if the branch
                    # endpoint still claims protected=true.
                    with self.assertRaises((attestor.Denied, AssertionError)):
                        self.evaluate(env, data)
                    continue
                if mode == "weak_owner":
                    p["required_pull_request_reviews"]["require_code_owner_reviews"] = False
                elif mode == "stale":
                    p["required_pull_request_reviews"]["dismiss_stale_reviews"] = False
                elif mode == "no_review":
                    p["required_pull_request_reviews"]["required_approving_review_count"] = 0
                elif mode == "bypass":
                    p["required_pull_request_reviews"]["bypass_pull_request_allowances"]["apps"] = [{"slug": "bypass"}]
                elif mode == "admin":
                    p["enforce_admins"]["enabled"] = False
                elif mode == "force_push":
                    p["allow_force_pushes"]["enabled"] = True
                elif mode == "delete":
                    p["allow_deletions"]["enabled"] = True
                elif mode == "no_checks":
                    del p["required_status_checks"]
                elif mode == "empty_checks":
                    p["required_status_checks"]["contexts"] = []
                elif mode == "non_strict_checks":
                    p["required_status_checks"]["strict"] = False
                elif mode == "invalid_checks":
                    p["required_status_checks"]["contexts"] = "bad"
                with self.assertRaises(attestor.Denied):
                    self.evaluate(env, data)

    def test_cgl_native_sanitizers_required_even_when_guest_flag_false(self):
        env, data = fixture(suite="cgl-compat", san=False)
        jobs = data["repos/" + repo + "/actions/runs/100/attempts/2/jobs?per_page=100"]["jobs"]
        source = jobs[0]
        required = "Execute linked CGL native tests with Clang ASan and UBSan"
        self.assertIn(required, attestor.required_steps("cgl-compat", False))
        self.assertIn(required, attestor.required_steps("cgl-compat", True))
        # A passing CGL guest smoke without native sanitizers must be denied.
        source["steps"] = [step for step in source["steps"] if step["name"] != required]
        with self.assertRaises(attestor.Denied):
            self.evaluate(env, data)

        env, data = fixture(suite="cgl-compat", san=False)
        steps = data["repos/" + repo + "/actions/runs/100/attempts/2/jobs?per_page=100"]["jobs"][0]["steps"]
        step = next(step for step in steps if step["name"] == required)
        step["conclusion"] = "skipped"
        with self.assertRaises(attestor.Denied):
            self.evaluate(env, data)

    def test_continuation_tsan_required_for_both_sanitizer_flags(self):
        name = "Execute exact-head pinned continuation under GCC TSan"
        for san in (True, False):
            with self.subTest(sanitizers=san):
                env, data = fixture(suite="continuation-gate", san=san)
                self.assertIn(name, attestor.required_steps("continuation-gate", san))
                source = data["repos/" + repo + "/actions/runs/100/attempts/2/jobs?per_page=100"]["jobs"][0]
                source["steps"] = [step for step in source["steps"] if step["name"] != name]
                with self.assertRaises(attestor.Denied):
                    self.evaluate(env, data)

                env, data = fixture(suite="continuation-gate", san=san)
                steps = data["repos/" + repo + "/actions/runs/100/attempts/2/jobs?per_page=100"]["jobs"][0]["steps"]
                next(step for step in steps if step["name"] == name)["conclusion"] = "skipped"
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


    def test_attestor_github_client_ignores_ambient_proxy_without_network(self):
        # This test never invokes the returned getter, so no network request
        # or real token is involved. The negative control must observe the
        # unsafe implicit proxy handler before checking our hardened opener.
        import os
        import urllib.request
        from unittest import mock

        original = urllib.request.build_opener
        with mock.patch.dict(os.environ, {
            "https_proxy": "http://127.0.0.1:19999",
            "http_proxy": "http://127.0.0.1:19999",
            "all_proxy": "http://127.0.0.1:19999",
        }):
            implicit = original(attestor.NoRedirect())
            implicit_proxies = [
                h.proxies for h in implicit.handlers
                if isinstance(h, urllib.request.ProxyHandler)
            ]
            self.assertTrue(any(p.get("https") for p in implicit_proxies),
                            "negative control did not inherit hostile proxy")
            with mock.patch.object(urllib.request, "build_opener",
                                   wraps=original) as spy:
                getter = attestor.github_fetch("synthetic-never-sent")
                self.assertTrue(callable(getter))
                passed = spy.call_args.args
                self.assertTrue(any(
                    type(h) is urllib.request.ProxyHandler and h.proxies == {}
                    for h in passed), "attestor did not explicitly disable proxies")
                self.assertTrue(any(type(h) is attestor.NoRedirect for h in passed),
                                "attestor lost its redirect rejection")


if __name__ == "__main__":
    unittest.main()
