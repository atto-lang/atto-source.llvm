#!/usr/bin/env python3
"""Independent no-PR-checkout post-run verifier. Source-org ephemeral runner only."""
import json
import os
import re
import urllib.error
import urllib.request

REPOSITORY = "ores-truffle-oreslang/oreslang-source.llvm"
WORKFLOW = ".github/workflows/llvm-source-exact-ephemeral.yml"
EXECUTION_LABEL = "oreslang-llvm-ephemeral-v1"
ATTESTOR_LABEL = "oreslang-llvm-attestor-ephemeral-v1"
SHA_RE = re.compile(r"[0-9a-f]{40}\Z")
PR_RE = re.compile(r"[1-9][0-9]{0,7}\Z")
COMMON = (
    "Verify open same-repo PR and exact immutable head before checkout",
    "Checkout trusted main for source-free workflow tests",
    "Execute trusted source-free post-run authorization regression mocks",
    "Fetch pinned PR objects into empty sparse checkout (never persist auth)",
    "Require exact tree and preinstalled native toolchain",
)
TOOLCHAIN = "Verify preinstalled LLVM / Clang / CMake toolchain"
ACTOR = (TOOLCHAIN, "Build actual Oreslang source compiler against LLVM",
         "Compile and execute real Oreslang source actor fixtures")
SUITES = {
    "actor-llvm": ACTOR,
    "actor-ledger": ACTOR + (
        "Run real ledger, scheduler STOP and pinned physical-heap native suite",
        "Execute real compiled Oreslang ledger guest fixtures",
    ),
    "continuation-gate": (
        TOOLCHAIN, "Build and execute exact-head pinned continuation CTest",
        "Execute exact-head pinned continuation GCC and Clang ASan UBSan",
    ),
    "cgl-compat": (
        TOOLCHAIN, "Execute exact-head CGL negative preflight, native compiler, CTest and guest ELF",
    ),
    "security-v3": ("Run full V3 private-export security regressions (no export)",),
}
SANITIZERS = {
    "actor-llvm": ("Repeat real source actor compile-and-execution under ASan/UBSan",),
    "actor-ledger": (
        "Repeat real source actor compile-and-execution under ASan/UBSan",
        "Execute compiled ledger guests under Clang ASan and UBSan",
    ),
    "cgl-compat": ("Execute linked CGL native tests with Clang ASan and UBSan",),
}


class Denied(ValueError):
    pass


def require(ok, reason):
    if not ok:
        raise Denied(reason)


def required_steps(suite, sanitizers):
    require(suite in SUITES, "invalid-suite")
    return COMMON + SUITES[suite] + (SANITIZERS.get(suite, ()) if sanitizers else ())


def verify_job_receipt(record, suite, sanitizers, attempt):
    require(type(record) is dict, "invalid-jobs-response")
    jobs = record.get("jobs")
    require(type(jobs) is list and record.get("total_count") == len(jobs) == 2,
            "unexpected-job-count-or-pagination")
    by_name = {job.get("name"): job for job in jobs if type(job) is dict}
    require(len(by_name) == 2 and set(by_name) ==
            {"source-exact-validation", "independent-postrun-attestation"},
            "unexpected-job-identity")
    source = by_name["source-exact-validation"]
    attestor = by_name["independent-postrun-attestation"]
    for job in (source, attestor):
        require(job.get("run_attempt") == attempt, "wrong-run-attempt")
        require(type(job.get("id")) is int and job["id"] > 0, "invalid-job-id")
        require(type(job.get("runner_id")) is int and job["runner_id"] > 0,
                "runner-not-assigned")
        require(type(job.get("runner_group_id")) is int and job["runner_group_id"] > 0,
                "runner-group-not-assigned")
        require(type(job.get("runner_name")) is str and job["runner_name"],
                "runner-name-missing")
    require(source["runner_id"] != attestor["runner_id"], "runner-reused")
    require(source.get("status") == "completed" and source.get("conclusion") == "success",
            "source-job-not-successful")
    require(attestor.get("status") == "in_progress", "attestor-not-in-progress")
    source_labels, attest_labels = source.get("labels"), attestor.get("labels")
    require(type(source_labels) is list and EXECUTION_LABEL in source_labels and
            ATTESTOR_LABEL not in source_labels, "wrong-source-runner-label")
    require(type(attest_labels) is list and ATTESTOR_LABEL in attest_labels and
            EXECUTION_LABEL not in attest_labels, "wrong-attestor-runner-label")
    steps = source.get("steps")
    require(type(steps) is list and len(steps) > 0, "source-steps-not-executed")
    indexed = {step.get("name"): step for step in steps if type(step) is dict}
    require(len(indexed) == len(steps), "duplicate-or-malformed-source-steps")
    for name in required_steps(suite, sanitizers):
        step = indexed.get(name)
        require(type(step) is dict and step.get("status") == "completed"
                and step.get("conclusion") == "success", "required-step-not-successful:" + name)
    return source["id"], attestor["id"]



def verify_strong_main_protection(protection):
    """Independent exact-head attestor must enforce the same source-main
    review and strict status policy as trusted preflight and JIT enrollment."""
    require(type(protection) is dict, "missing-main-protection")
    reviews = protection.get("required_pull_request_reviews")
    checks = protection.get("required_status_checks")
    require(type(reviews) is dict and type(checks) is dict,
            "missing-approvals-or-required-status-checks")
    require(reviews.get("require_code_owner_reviews") is True
            and reviews.get("dismiss_stale_reviews") is True
            and type(reviews.get("required_approving_review_count")) is int
            and reviews["required_approving_review_count"] >= 1,
            "weak-independent-review-policy")
    require(protection.get("enforce_admins", {}).get("enabled") is True
            and protection.get("allow_force_pushes", {}).get("enabled") is False
            and protection.get("allow_deletions", {}).get("enabled") is False,
            "admin-force-or-delete-bypass")
    require(checks.get("strict") is True, "non-strict-required-checks")
    contexts, identities = checks.get("contexts", []), checks.get("checks", [])
    require(type(contexts) is list and type(identities) is list
            and len(contexts) + len(identities) > 0,
            "missing-required-status-checks")
    bypass = reviews.get("bypass_pull_request_allowances") or {}
    require(type(bypass) is dict and all(bypass.get(k, []) == []
            for k in ("users", "teams", "apps")), "independent-review-bypass")
    return True

def verify_live(get_json, env):
    require(env.get("GITHUB_REPOSITORY") == REPOSITORY, "foreign-repository")
    require(env.get("GITHUB_REF") == "refs/heads/main", "not-trusted-main-event")
    require(env.get("RUNNER_ENVIRONMENT") == "self-hosted", "not-self-hosted")
    source_sha, main_sha = env.get("SOURCE_SHA", ""), env.get("GITHUB_SHA", "")
    require(bool(SHA_RE.fullmatch(source_sha)) and bool(SHA_RE.fullmatch(main_sha)),
            "invalid-source-or-dispatch-sha")
    pr = env.get("PR_NUMBER", "")
    require(bool(PR_RE.fullmatch(pr)), "invalid-pr-number")
    suite = env.get("SUITE", "")
    require(suite in SUITES, "invalid-suite")
    require(env.get("RUN_SANITIZERS") in ("true", "false"), "invalid-sanitizer-flag")
    sanitizers = env["RUN_SANITIZERS"] == "true"
    run_id, attempt = env.get("GITHUB_RUN_ID", ""), env.get("GITHUB_RUN_ATTEMPT", "")
    require(run_id.isdigit() and int(run_id) > 0 and attempt.isdigit() and
            int(attempt) > 0, "invalid-run-identity")
    prefix = "repos/" + REPOSITORY
    run = get_json(prefix + "/actions/runs/" + run_id)
    require(run.get("event") == "workflow_dispatch" and run.get("head_sha") == main_sha
            and run.get("head_branch") == "main" and run.get("path") == WORKFLOW,
            "wrong-dispatch-context")
    main = get_json(prefix + "/branches/main")
    require(main.get("protected") is True and main.get("commit", {}).get("sha") == main_sha,
            "main-unprotected-or-moved")
    verify_strong_main_protection(get_json(prefix + "/branches/main/protection"))
    pull = get_json(prefix + "/pulls/" + pr)
    head, author = pull.get("head", {}), pull.get("user", {})
    require(head.get("sha") == source_sha
            and head.get("repo", {}).get("full_name") == REPOSITORY
            and head.get("repo", {}).get("private") is True
            and pull.get("state") == "open", "pr-head-or-source-boundary-changed")
    require(type(author.get("id")) is int and author["id"] > 0
            and author["id"] != 11139560, "missing-independent-author")
    reviews = get_json(prefix + "/pulls/" + pr + "/reviews?per_page=100")
    require(type(reviews) is list and len(reviews) < 100, "reviews-paginated-or-invalid")
    independent = [review for review in reviews
                   if review.get("user", {}).get("login") == "ORESoftware"
                   and review.get("submitted_at")]
    require(independent, "no-independent-review")
    latest = sorted(independent, key=lambda review:
                    (review["submitted_at"], review.get("id", 0)))[-1]
    require(latest.get("state") == "APPROVED" and latest.get("commit_id") == source_sha
            and latest.get("user", {}).get("id") == 11139560,
            "stale-dismissed-or-wrong-review")
    jobs = get_json(prefix + "/actions/runs/" + run_id + "/attempts/" + attempt
                    + "/jobs?per_page=100")
    return verify_job_receipt(jobs, suite, sanitizers, int(attempt))


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        raise Denied("unexpected-api-redirect")


def github_fetch(token):
    opener = urllib.request.build_opener(NoRedirect())

    def get_json(path):
        request = urllib.request.Request("https://api.github.com/" + path, headers={
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "Authorization": "Bearer " + token,
        })
        with opener.open(request, timeout=15) as response:
            raw = response.read(2 * 1024 * 1024 + 1)
        require(len(raw) <= 2 * 1024 * 1024, "api-response-too-large")
        return json.loads(raw)

    return get_json


def main():
    token = os.environ.get("GH_TOKEN", "")
    require(bool(token), "missing-read-only-attestor-token")
    execution_id, attestor_id = verify_live(github_fetch(token), os.environ)
    print("FRESH_SOURCE_ORG_INDEPENDENT_ATTESTOR=PASS")
    print("EXECUTION_JOB_ID=" + str(execution_id))
    print("ATTESTOR_JOB_ID=" + str(attestor_id))
    print("MANUAL_MAIN_DISPATCH_NOT_PR_HEAD_REQUIRED_STATUS=true")


if __name__ == "__main__":
    try:
        main()
    except (Denied, urllib.error.URLError, ValueError, KeyError, TypeError) as error:
        reason = str(error) if isinstance(error, Denied) else "unavailable-or-invalid-api-response"
        print("FRESH_SOURCE_ORG_INDEPENDENT_ATTESTOR=DENIED:" + reason)
        raise SystemExit(1)
