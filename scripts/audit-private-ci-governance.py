#!/usr/bin/env python3
"""Source-free policy audit, NOT authorization to export private source.

Only an independently reviewed protected-main publisher may export private
source. This preflight checks local policy structure and, with --live, rejects
unsuitable current source visibility/branch protection. No writes or transfers.
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
APPROVAL_PATH = Path(".github/private-ci-approved-repositories.txt")
OWNERS_PATH = Path(".github/CODEOWNERS")
DOC_PATH = Path("docs/security/CI_PRIVATE_SOURCE_MIRRORING.md")
REQUIRED_OWNERS = {
    "/.github/CODEOWNERS",
    "/.github/workflows/*",
    "/.github/private-ci-approved-repositories.txt",
    "/docs/security/CI_PRIVATE_SOURCE_MIRRORING.md",
    "/scripts/audit-private-ci-governance.py",
    "/scripts/test-audit-private-ci-governance.py",
    "/scripts/run-in-private-ci-destination.sh",
    "/scripts/publish-private-ci-source.py",
    "/scripts/test-private-ci-destination.sh",
    "/scripts/test-publish-private-ci-source.py",
}
DESTINATION = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z", re.ASCII)


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
    for phrase in ("protected", "private", "tree", "canary", "Direct GitHub connector"):
        if phrase not in doc:
            raise GovernanceError(f"missing documented security condition: {phrase}")
    return approved


def check_live_metadata(repo_data: dict, branch_data: dict, protection_data: dict) -> None:
    if repo_data.get("full_name") != SOURCE or repo_data.get("private") is not True:
        raise GovernanceError("source identity or visibility is not private and exact")
    if repo_data.get("default_branch") != "main":
        raise GovernanceError("unexpected default branch")
    if branch_data.get("name") != "main" or branch_data.get("protected") is not True:
        raise GovernanceError("protected main is not enforced")
    reviews = protection_data.get("required_pull_request_reviews") or {}
    if reviews.get("require_code_owner_reviews") is not True:
        raise GovernanceError("required CODEOWNER reviews not enforced")
    if reviews.get("dismiss_stale_reviews") is not True:
        raise GovernanceError("stale reviews are not dismissed")
    if (protection_data.get("enforce_admins") or {}).get("enabled") is not True:
        raise GovernanceError("administrator protection not enforced")


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
                        help="require live source identity and protected-main checks")
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
            print("Live source and minimum protection verified; independent approval still required.")
        return 0
    except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as exc:
        print(f"DENIED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
