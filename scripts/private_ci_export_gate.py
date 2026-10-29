#!/usr/bin/env python3
"""Fail-closed authorization preflight for cross-organization private CI export.

The policy root must be an independently reviewed checkout of protected MAIN,
not the feature/PR branch being exported. This script neither exports source nor
verifies GitHub administrator settings; both are independent requirements.
"""
import argparse
import json
import re
import sys
from pathlib import Path

REPO = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")


class GateRejected(ValueError):
    pass


def _read(root: Path, name: str) -> str:
    current = root
    if current.is_symlink():
        raise GateRejected("policy root is a symlink")
    for part in name.split("/"):
        if part in ("", ".", ".."):
            raise GateRejected("unsafe policy path")
        current = current / part
        if current.is_symlink():
            raise GateRejected("policy path contains a symlink")
    if not current.is_file():
        raise GateRejected("required policy file missing")
    return current.read_text(encoding="utf-8")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise GateRejected("duplicate JSON policy key")
        result[key] = value
    return result


def _sha(value):
    return isinstance(value, str) and SHA.fullmatch(value) is not None


def authorize(root: Path, destination: str, repository_id: int, base_sha: str,
              workflow_path: str, workflow_blob_sha: str, plan_path: str,
              plan_blob_sha: str, pr_number: int) -> None:
    if not isinstance(destination, str) or not REPO.fullmatch(destination):
        raise GateRejected("invalid exact destination")
    if type(repository_id) is not int or repository_id <= 0:
        raise GateRejected("missing immutable repository ID")
    if type(pr_number) is not int or pr_number <= 0:
        raise GateRejected("invalid PR number")
    if not all(map(_sha, (base_sha, workflow_blob_sha, plan_blob_sha))):
        raise GateRejected("invalid immutable Git object SHA")
    for path in (workflow_path, plan_path):
        if not isinstance(path, str) or path.startswith("/") or "\" in path or any(
                x in ("", ".", "..") for x in path.split("/")):
            raise GateRejected("unsafe workflow or plan path")

    allowlist = _read(root, ".github/private-ci-approved-repositories.txt")
    approved = set()
    for line in allowlist.splitlines():
        item = line.strip()
        if not item or item.startswith("#"):
            continue
        if item != line or not REPO.fullmatch(item) or item in approved:
            raise GateRejected("invalid or duplicate private carrier allowlist entry")
        approved.add(item)
    if destination not in approved:
        raise GateRejected("destination not approved on protected main")

    contract = json.loads(_read(root, ".github/private-ci-carrier-contracts.json"),
                          object_pairs_hook=_unique_object)
    if contract.get("schema") != 1 or not isinstance(contract.get("carriers"), dict):
        raise GateRejected("invalid private carrier policy schema")
    carrier = contract["carriers"].get(destination)
    if not isinstance(carrier, dict):
        raise GateRejected("missing exact destination contract")
    if (type(carrier.get("repository_id")) is not int or
            carrier["repository_id"] != repository_id or
            carrier.get("base_sha") != base_sha or
            carrier.get("workflow_path") != workflow_path or
            carrier.get("workflow_blob_sha") != workflow_blob_sha):
        raise GateRejected("carrier identity/workflow pin mismatch")
    plans = carrier.get("test_plans")
    plan = plans.get(plan_path) if isinstance(plans, dict) else None
    if (not isinstance(plan, dict) or plan.get("blob_sha") != plan_blob_sha or
            not isinstance(plan.get("allowed_prs"), list) or
            any(type(x) is not int or x <= 0 for x in plan["allowed_prs"]) or
            pr_number not in plan["allowed_prs"]):
        raise GateRejected("missing exact pinned test plan for PR")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--policy-root", required=True, type=Path)
    parser.add_argument("--destination", required=True)
    parser.add_argument("--repository-id", required=True, type=int)
    parser.add_argument("--base-sha", required=True)
    parser.add_argument("--workflow-path", required=True)
    parser.add_argument("--workflow-blob-sha", required=True)
    parser.add_argument("--plan-path", required=True)
    parser.add_argument("--plan-blob-sha", required=True)
    parser.add_argument("--pr", required=True, type=int)
    args = parser.parse_args(argv)
    try:
        authorize(args.policy_root, args.destination, args.repository_id,
                  args.base_sha, args.workflow_path, args.workflow_blob_sha,
                  args.plan_path, args.plan_blob_sha, args.pr)
    except (GateRejected, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"PRIVATE_SOURCE_EXPORT_DENIED: {exc}", file=sys.stderr)
        return 1
    print("PRIVATE_SOURCE_EXPORT_POLICY_MATCH (reviewed-main and GitHub settings still required)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
