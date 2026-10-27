#!/usr/bin/env python3
"""Source-free policy audit, NOT authorization to export private source.

Only the V3 publisher may authorize export. Source export always requires
enforced protected-main governance. This preflight checks local policy
structure; --live diagnoses that mandatory source gate. V3 may separately use
live protected carrier governance. No writes or transfers.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import urllib.error
import urllib.request

SOURCE = "ores-truffle-oreslang/oreslang-source.llvm"
SOURCE_ID = 1410897874
APPROVAL_PATH = Path(".github/private-ci-approved-repositories.txt")
OWNERS_PATH = Path(".github/CODEOWNERS")
DOC_PATH = Path("docs/security/CI_PRIVATE_SOURCE_MIRRORING.md")
REQUIRED_OWNERS = {
    "/.github/CODEOWNERS",
    "/.github/workflows/*",
    "/.github/private-ci-approved-repositories.txt",
    "/.github/private-ci-carrier-contracts.json",
    "/docs/security/CI_PRIVATE_SOURCE_MIRRORING.md",
    "/scripts/audit-private-ci-governance.py",
    "/scripts/test-audit-private-ci-governance.py",
    "/scripts/run-in-private-ci-destination.sh",
    "/scripts/publish-private-ci-source.py",
    "/scripts/publish-private-ci-source-v3.py",
    "/scripts/test-publish-private-ci-source-v3.py",
    "/scripts/test-private-ci-destination.sh",
    "/scripts/test-publish-private-ci-source.py",
}
DESTINATION = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z", re.ASCII)
SHA40 = re.compile(r"[0-9a-f]{40}\Z", re.ASCII)
GITHUB_ACTIONS_APP_ID = 15368
SOURCE_POLICY_CHECK = "private-ci-governance"


class GovernanceError(ValueError):
    pass


def parse_allowlist(content: str) -> tuple[str, ...]:
    result = []
    for number, line in enumerate(content.splitlines(), start=1):
        if not line or line.startswith("#"):
            continue
        if not DESTINATION.fullmatch(line):
            raise GovernanceError(f"invalid destination policy line {number}")
        if any(part in (".", "..") for part in line.split("/")):
            raise GovernanceError(f"invalid destination path component at line {number}")
        if line.casefold() == SOURCE.casefold():
            raise GovernanceError("source repository cannot be its own destination")
        if any(line.casefold() == old.casefold() for old in result):
            raise GovernanceError("duplicate destination")
        result.append(line)
    return tuple(result)


def check_policy_files(root: Path) -> tuple[str, ...]:
    root = root.resolve(strict=True)
    approved = parse_allowlist((root / APPROVAL_PATH).read_text(encoding="utf-8"))
    owners_text = (root / OWNERS_PATH).read_text(encoding="utf-8")
    rules = {}
    for line in owners_text.splitlines():
        clean = line.strip()
        if not clean or clean.startswith("#"):
            continue
        parts = clean.split()
        if len(parts) >= 2:
            rules[parts[0]] = parts[1:]
    for path in REQUIRED_OWNERS:
        if "@ORESoftware" not in rules.get(path, []):
            raise GovernanceError(f"missing exact CODEOWNER: {path}")
    doc = (root / DOC_PATH).read_text(encoding="utf-8")
    for phrase in (
        "protected", "private", "tree", "canary",
        "Direct GitHub connector", "last-push-independent",
        "GitHub Actions App ID", "no review/tree-attestation substitute",
    ):
        if phrase not in doc:
            raise GovernanceError(f"missing documented security condition: {phrase}")
    return approved


def check_live_metadata(repo_data: dict, branch_data: dict, protection_data: dict) -> None:
    if not all(isinstance(value, dict) for value in
               (repo_data, branch_data, protection_data)):
        raise GovernanceError("GitHub protection metadata has invalid object shape")
    if (
        repo_data.get("full_name") != SOURCE
        or repo_data.get("id") != SOURCE_ID
        or repo_data.get("private") is not True
        or repo_data.get("visibility") != "private"
        or repo_data.get("fork") is not False
        or repo_data.get("archived") is not False
        or repo_data.get("disabled") is not False
    ):
        raise GovernanceError("source identity/state is not private, exact, and active")
    if repo_data.get("default_branch") != "main":
        raise GovernanceError("unexpected default branch")

    if branch_data.get("name") != "main" or branch_data.get("protected") is not True:
        raise GovernanceError("protected main is not enforced")
    commit = branch_data.get("commit")
    if (not isinstance(commit, dict)
            or not isinstance(commit.get("sha"), str)
            or not SHA40.fullmatch(commit["sha"])):
        raise GovernanceError("protected main commit SHA is missing or invalid")

    reviews = protection_data.get("required_pull_request_reviews")
    checks = protection_data.get("required_status_checks")
    conversations = protection_data.get("required_conversation_resolution")
    if not isinstance(reviews, dict):
        raise GovernanceError("pull-request review protection missing")
    if reviews.get("require_code_owner_reviews") is not True:
        raise GovernanceError("required CODEOWNER reviews not enforced")
    if reviews.get("dismiss_stale_reviews") is not True:
        raise GovernanceError("stale reviews are not dismissed")
    if reviews.get("require_last_push_approval") is not True:
        raise GovernanceError("last-push-independent approval not enforced")

    approval_count = reviews.get("required_approving_review_count")
    if type(approval_count) is not int or approval_count < 1:
        raise GovernanceError("positive approving review count not enforced")

    bypass = reviews.get("bypass_pull_request_allowances")
    if (not isinstance(bypass, dict)
            or set(bypass) != {"users", "teams", "apps"}
            or any(not isinstance(bypass.get(kind), list)
                   or bypass.get(kind) for kind in ("users", "teams", "apps"))):
        raise GovernanceError("review bypass policy is missing, malformed or allows bypass")

    if (not isinstance(checks, dict)
            or checks.get("strict") is not True):
        raise GovernanceError("strict required status checks not enforced")
    contexts = checks.get("contexts", [])
    named = checks.get("checks")
    if (not isinstance(contexts, list)
            or any(not isinstance(name, str) or not name.strip()
                   for name in contexts)
            or len(contexts) != len(set(contexts))
            or not isinstance(named, list)
            or not named):
        raise GovernanceError("required CI issuer binding missing or malformed")
    seen = set()
    for item in named:
        if not isinstance(item, dict):
            raise GovernanceError("required CI check metadata malformed")
        name = item.get("context")
        app_id = item.get("app_id")
        if (not isinstance(name, str) or not name.strip() or name in seen):
            raise GovernanceError("required CI check identity ambiguous")
        if type(app_id) is not int or app_id != GITHUB_ACTIONS_APP_ID:
            raise GovernanceError("required CI check issuer is not GitHub Actions")
        seen.add(name)
    if any(name not in seen for name in contexts):
        raise GovernanceError("legacy required CI context lacks issuer binding")
    if SOURCE_POLICY_CHECK not in seen:
        raise GovernanceError("required private-CI governance check missing")

    if (not isinstance(conversations, dict)
            or conversations.get("enabled") is not True):
        raise GovernanceError("conversation resolution not enforced")

    admins = protection_data.get("enforce_admins")
    if not isinstance(admins, dict) or admins.get("enabled") is not True:
        raise GovernanceError("administrator protection not enforced")

    force_pushes = protection_data.get("allow_force_pushes")
    if not isinstance(force_pushes, dict) or force_pushes.get("enabled") is not False:
        raise GovernanceError("force pushes are not explicitly forbidden")

    deletions = protection_data.get("allow_deletions")
    if not isinstance(deletions, dict) or deletions.get("enabled") is not False:
        raise GovernanceError("branch deletion is not explicitly forbidden")

def _github_get(path: str, token: str) -> dict:
    req = urllib.request.Request(
        f"https://api.github.com/repos/{SOURCE}/{path}",
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--live", action="store_true",
                        help="diagnose mandatory source protected-main mode; V3 separately requires protected carrier main")
    args = parser.parse_args()
    try:
        approved = check_policy_files(args.root)
        print(f"Policy structure valid; {len(approved)} destinations listed (NOT authorized by this check).")
        if args.live:
            token = os.environ.get("GITHUB_TOKEN", "")
            if not token:
                raise GovernanceError("GITHUB_TOKEN missing; fail closed")
            check_live_metadata(
                _github_get("", token),
                _github_get("branches/main", token),
                _github_get("branches/main/protection", token),
            )
            print("Mandatory source protected-main mode verified; protected carrier governance is validated separately by V3.")
        return 0
    except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as exc:
        print(f"DENIED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
