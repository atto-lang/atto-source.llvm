#!/usr/bin/env python3
"""Source-free policy audit, NOT authorization to export private source.

Only the V3 publisher may authorize export. Source export always requires
enforced protected-main governance. This preflight checks local policy
structure; --live diagnoses that mandatory source gate. V3 may separately use
exact review attestation for an unprotected private carrier. No writes or
transfers.
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
        "Direct GitHub connector", "review-attested",
    ):
        if phrase not in doc:
            raise GovernanceError(f"missing documented security condition: {phrase}")
    return approved


def check_live_metadata(repo_data: dict, branch_data: dict, protection_data: dict) -> None:
    if (
        repo_data.get("full_name") != SOURCE
        or repo_data.get("id") != SOURCE_ID
        or repo_data.get("private") is not True
    ):
        raise GovernanceError("source identity or visibility is not private and exact")
    if repo_data.get("default_branch") != "main":
        raise GovernanceError("unexpected default branch")
    if repo_data.get("archived") is True or repo_data.get("disabled") is True:
        raise GovernanceError("source repository is not active")

    if branch_data.get("name") != "main" or branch_data.get("protected") is not True:
        raise GovernanceError("protected main is not enforced")
    commit = branch_data.get("commit") or {}
    if not isinstance(commit, dict) or not SHA40.fullmatch(str(commit.get("sha", ""))):
        raise GovernanceError("protected main commit SHA is missing or invalid")

    reviews = protection_data.get("required_pull_request_reviews")
    if not isinstance(reviews, dict):
        raise GovernanceError("pull-request review protection missing")
    if reviews.get("require_code_owner_reviews") is not True:
        raise GovernanceError("required CODEOWNER reviews not enforced")
    if reviews.get("dismiss_stale_reviews") is not True:
        raise GovernanceError("stale reviews are not dismissed")

    approval_count = reviews.get("required_approving_review_count")
    if type(approval_count) is not int or approval_count < 1:
        raise GovernanceError("positive approving review count not enforced")

    bypass = reviews.get("bypass_pull_request_allowances")
    if not isinstance(bypass, dict):
        raise GovernanceError("review bypass policy unavailable")
    if any(bypass.get(kind) for kind in ("users", "teams", "apps")):
        raise GovernanceError("review bypass allowances are not empty")

    if (protection_data.get("enforce_admins") or {}).get("enabled") is not True:
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
                        help="diagnose mandatory source protected-main mode; V3 separately verifies carrier review attestation")
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
            print("Mandatory source protected-main mode verified; carrier review attestation, if used, is validated separately by V3.")
        return 0
    except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as exc:
        print(f"DENIED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
