#!/usr/bin/env python3
"""Fail-closed authorization preflight for cross-organization private CI export.

The policy root must be an independently reviewed checkout of protected MAIN,
not the feature/PR branch being exported. This script neither exports source nor
verifies GitHub administrator settings; both are independent requirements.
"""
import argparse
import json
import math
import os
import re
import sys
import stat
from pathlib import Path

REPO = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")


MAX_POLICY_BYTES = 1024 * 1024


class GateRejected(ValueError):
    pass


def _read(root: Path, name: str) -> str:
    """Open protected policy objects by descriptor, never following symlinks.

    Opening each ancestor relative to its verified directory descriptor also
    avoids an lstat->open swap of a parent, policy directory or final file.
    """
    if not all(hasattr(os, flag) for flag in ("O_NOFOLLOW", "O_DIRECTORY")):
        raise GateRejected("no symlink-safe policy file opening available")
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    file_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
    absolute = root.absolute()
    parents = list(absolute.parts[1:])
    parts = name.split("/")
    if any(part in ("", ".", "..") for part in parents + parts):
        raise GateRejected("unsafe policy path")

    directory_fd = os.open("/", directory_flags)
    try:
        for part in parents + parts[:-1]:
            next_fd = os.open(part, directory_flags, dir_fd=directory_fd)
            os.close(directory_fd)
            directory_fd = next_fd
        file_fd = os.open(parts[-1], file_flags, dir_fd=directory_fd)
        with os.fdopen(file_fd, "rb") as stream:
            info = os.fstat(stream.fileno())
            if not stat.S_ISREG(info.st_mode):
                raise GateRejected("policy is not a regular file")
            if info.st_size > MAX_POLICY_BYTES:
                raise GateRejected("policy file exceeds byte bound")
            data = stream.read(MAX_POLICY_BYTES + 1)
        if len(data) > MAX_POLICY_BYTES:
            raise GateRejected("policy file exceeds byte bound")
        return data.decode("utf-8")
    except OSError as exc:
        raise GateRejected("policy file cannot be opened without following symlinks") from exc
    finally:
        os.close(directory_fd)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise GateRejected("duplicate JSON policy key")
        result[key] = value
    return result


def _reject_nonfinite(value):
    raise GateRejected("non-finite JSON policy value")


def _finite_float(value):
    parsed = float(value)
    if not math.isfinite(parsed):
        raise GateRejected("non-finite JSON policy number")
    return parsed


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
    for path, prefix, suffix in (
            (workflow_path, ".github/workflows/", ".yml"),
            (plan_path, "ci/test-plans/", ".json")):
        if (not isinstance(path, str) or not path.startswith(prefix)
                or not path.endswith(suffix) or path.startswith("/")
                or not re.fullmatch(r"[A-Za-z0-9._/-]+", path)
                or any(segment in ("", ".", "..", ".git")
                       for segment in path.split("/"))):
            raise GateRejected("unsafe workflow or plan path")

    allowlist = _read(root, ".github/private-ci-approved-repositories.txt")
    approved = set()
    for line in allowlist.splitlines():
        item = line.strip()
        if not item or item.startswith("#"):
            continue
        if item != line or not REPO.fullmatch(item) or item.casefold() in approved:
            raise GateRejected("invalid or duplicate private carrier allowlist entry")
        approved.add(item.casefold())
    if destination.casefold() not in approved:
        raise GateRejected("destination not approved on protected main")

    contract = json.loads(_read(root, ".github/private-ci-carrier-contracts.json"),
                          object_pairs_hook=_unique_object, parse_constant=_reject_nonfinite,
                          parse_float=_finite_float)
    if (not isinstance(contract, dict) or type(contract.get("schema")) is not int or
            contract["schema"] != 1 or
            not isinstance(contract.get("carriers"), dict)):
        raise GateRejected("invalid private carrier policy schema")
    # GitHub repository identities are case-insensitive. Reject ambiguous
    # contracts instead of letting JSON insertion order select an identity.
    normalized = set()
    for key in contract["carriers"]:
        if not isinstance(key, str) or not REPO.fullmatch(key) or key.casefold() in normalized:
            raise GateRejected("invalid or case-duplicate private carrier contract key")
        normalized.add(key.casefold())
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
            len(set(plan["allowed_prs"])) != len(plan["allowed_prs"]) or
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
