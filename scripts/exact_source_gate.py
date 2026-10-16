#!/usr/bin/env python3
"""Fail-closed, offline CI proof for exact Git-source mirrors.

Inputs are GitHub REST tree JSON, run JSON, and job JSON. The caller must
retrieve these from the GitHub API; this verifier neither fetches credentials
nor treats a zero-step job, stale head, or partial source mirror as success.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SHA = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})")


class GateError(ValueError):
    pass


def git_files(tree: dict, prefix: str = "") -> dict[str, tuple[str, str, str]]:
    if not isinstance(tree, dict) or tree.get("truncated") is not False:
        raise GateError("Git tree response absent or truncated")
    entries = tree.get("tree")
    if not isinstance(entries, list):
        raise GateError("Git tree response has no entry array")
    found: dict[str, tuple[str, str, str]] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise GateError("malformed Git tree entry")
        path, kind, sha, mode = (entry.get(k) for k in ("path", "type", "sha", "mode"))
        if not isinstance(path, str) or not isinstance(kind, str):
            raise GateError("malformed Git tree entry")
        if kind == "tree":
            continue
        if kind not in ("blob", "commit") or not isinstance(mode, str):
            raise GateError("unexpected non-file Git tree entry")
        if not isinstance(sha, str) or not SHA.fullmatch(sha):
            raise GateError("invalid Git entry SHA")
        if prefix and not path.startswith(prefix):
            continue
        relative = path[len(prefix):]
        if not relative or relative.startswith("/") or ".." in relative.split("/"):
            raise GateError("unsafe or invalid relative file path")
        if relative in found:
            raise GateError("duplicate file path")
        found[relative] = (kind, sha, mode)
    if not found:
        raise GateError("empty file set or missing mirror prefix")
    return found


def verify_trees(source: dict, mirror: dict, prefix: str) -> int:
    if not prefix.endswith("/") or prefix.startswith("/") or ".." in prefix.split("/"):
        raise GateError("mirror prefix must be a relative directory ending with /")
    original = git_files(source)
    copied = git_files(mirror, prefix)
    missing = sorted(set(original) - set(copied))
    extra = sorted(set(copied) - set(original))
    changed = sorted(p for p in set(original) & set(copied) if original[p] != copied[p])
    if missing or extra or changed:
        raise GateError(
            f"exact-source mismatch: missing={missing[:8]} extra={extra[:8]} changed={changed[:8]}")
    return len(original)


def verify_workflows(evidence: list[dict], mirror_head: str,
                     required_jobs: list[str], required_steps: list[str]) -> int:
    if not SHA.fullmatch(mirror_head) or not evidence:
        raise GateError("invalid mirror head or missing executed workflow evidence")
    if not required_jobs or not required_steps:
        raise GateError("explicit required jobs and test steps are mandatory")
    job_names: set[str] = set()
    step_names: set[str] = set()
    count = 0
    seen_jobs: set[int] = set()
    for bundle in evidence:
        if not isinstance(bundle, dict):
            raise GateError("malformed run evidence")
        run = bundle.get("run")
        jobs = bundle.get("jobs")
        if not isinstance(run, dict) or not isinstance(jobs, list) or not jobs:
            raise GateError("run missing job records")
        run_id, attempt = run.get("id"), run.get("run_attempt")
        if type(run_id) is not int or run_id <= 0 or type(attempt) is not int or attempt <= 0:
            raise GateError("run evidence missing valid run ID or attempt")
        if run.get("head_sha") != mirror_head:
            raise GateError("stale or mismatched workflow head")
        if run.get("status") != "completed" or run.get("conclusion") != "success":
            raise GateError("workflow did not complete successfully")
        for job in jobs:
            if not isinstance(job, dict) or not isinstance(job.get("name"), str):
                raise GateError("malformed job evidence")
            job_id = job.get("id")
            if type(job_id) is not int or job_id <= 0 or job_id in seen_jobs:
                raise GateError("invalid or duplicate job ID")
            seen_jobs.add(job_id)
            if (type(job.get("run_id")) is not int or job["run_id"] != run_id
                    or type(job.get("run_attempt")) is not int or job["run_attempt"] != attempt
                    or job.get("head_sha") != mirror_head):
                raise GateError("job does not belong to the evidenced run, attempt and head")
            if job.get("status") != "completed" or job.get("conclusion") != "success":
                raise GateError(f"failed/skipped/incomplete job: {job['name']}")
            steps = job.get("steps")
            if not isinstance(steps, list) or not steps:
                raise GateError(f"job had zero executed steps: {job['name']}")
            successful = [
                s["name"] for s in steps
                if isinstance(s, dict) and isinstance(s.get("name"), str)
                and s.get("status") == "completed" and s.get("conclusion") == "success"
            ]
            if not successful:
                raise GateError(f"job never executed a successful step: {job['name']}")
            job_names.add(job["name"])
            step_names.update(successful)
            count += 1
    if set(required_jobs) - job_names:
        raise GateError(f"missing required jobs: {sorted(set(required_jobs)-job_names)}")
    if set(required_steps) - step_names:
        raise GateError(f"missing executed test steps: {sorted(set(required_steps)-step_names)}")
    return count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-tree", required=True, type=Path)
    parser.add_argument("--mirror-tree", required=True, type=Path)
    parser.add_argument("--mirror-prefix", required=True)
    parser.add_argument("--mirror-head", required=True)
    parser.add_argument("--workflow-evidence", required=True, action="append", type=Path)
    parser.add_argument("--require-job", required=True, action="append")
    parser.add_argument("--require-step", required=True, action="append")
    args = parser.parse_args(argv)
    try:
        count = verify_trees(
            json.loads(args.source_tree.read_text()),
            json.loads(args.mirror_tree.read_text()), args.mirror_prefix)
        jobs = verify_workflows(
            [json.loads(path.read_text()) for path in args.workflow_evidence],
            args.mirror_head, args.require_job, args.require_step)
    except (GateError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"REJECT: {error}", file=sys.stderr)
        return 1
    print(f"PASS: {count} exact Git objects; {jobs} executed CI jobs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
