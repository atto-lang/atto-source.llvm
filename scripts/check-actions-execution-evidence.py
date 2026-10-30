#!/usr/bin/env python3
"""Fail-closed GitHub Actions execution evidence gate.

Consumes authentic GitHub REST run and job snapshots captured by a trusted
caller. It never reaches GitHub, loads tokens, or authorizes source export.
"""
import argparse
import json
import re
import sys
from pathlib import Path

SHA40 = re.compile(r"[0-9a-f]{40}\Z")


class EvidenceError(ValueError):
    pass


def verify(run, jobs_data, *, expected_sha, expected_run_id, expected_attempt,
           expected_job_count, required_steps):
    if not isinstance(run, dict) or not isinstance(jobs_data, dict):
        raise EvidenceError("run and jobs must be JSON objects")
    if not isinstance(expected_sha, str) or not SHA40.fullmatch(expected_sha):
        raise EvidenceError("invalid pinned SHA-40")
    if type(expected_run_id) is not int or expected_run_id <= 0:
        raise EvidenceError("invalid expected run ID")
    if type(expected_attempt) is not int or expected_attempt <= 0:
        raise EvidenceError("invalid expected attempt")
    if type(expected_job_count) is not int or expected_job_count <= 0:
        raise EvidenceError("invalid expected job count")
    if not required_steps or any(not isinstance(s, str) or not s.strip() or s != s.strip()
                                 for s in required_steps) or len(set(required_steps)) != len(required_steps):
        raise EvidenceError("required step names must be explicit, unique and canonical")
    if any(s in ("Set up job", "Complete job") or s.startswith("Post ")
           for s in required_steps):
        raise EvidenceError("setup and cleanup steps are not test execution evidence")
    if (type(run.get("id")) is not int or type(run.get("run_attempt")) is not int
            or run.get("id") != expected_run_id or run.get("head_sha") != expected_sha
            or run.get("run_attempt") != expected_attempt):
        raise EvidenceError("run identity, attempt, or head SHA mismatch")
    if run.get("status") != "completed" or run.get("conclusion") != "success":
        raise EvidenceError("run has no successful completed execution")
    jobs = jobs_data.get("jobs")
    if (type(jobs_data.get("total_count")) is not int
            or jobs_data["total_count"] != expected_job_count
            or not isinstance(jobs, list) or len(jobs) != expected_job_count):
        raise EvidenceError("job list incomplete, truncated, or unexpected matrix size")
    seen_ids = set()
    successful_steps = set(required_steps)
    for job in jobs:
        if not isinstance(job, dict):
            raise EvidenceError("job record not an object")
        jid = job.get("id")
        if type(jid) is not int or jid <= 0 or jid in seen_ids:
            raise EvidenceError("missing or duplicate job ID")
        seen_ids.add(jid)
        if (type(job.get("run_id")) is not int or type(job.get("run_attempt")) is not int
                or job.get("run_id") != expected_run_id or job.get("head_sha") != expected_sha
                or job.get("run_attempt") != expected_attempt):
            raise EvidenceError("job not pinned to exact run/attempt/head")
        if job.get("status") != "completed" or job.get("conclusion") != "success":
            raise EvidenceError("job failed or has not completed")
        if (type(job.get("runner_id")) is not int or job["runner_id"] <= 0
                or not isinstance(job.get("runner_name"), str)
                or not job["runner_name"].strip()):
            raise EvidenceError("job had no assigned real runner")
        steps = job.get("steps")
        if not isinstance(steps, list) or not steps:
            raise EvidenceError("job has zero recorded execution steps")
        observed = set()
        job_successful_steps = set()
        for step in steps:
            if not isinstance(step, dict) or not isinstance(step.get("name"), str):
                raise EvidenceError("invalid step record")
            name = step["name"]
            if name in observed:
                raise EvidenceError("ambiguous duplicate step name")
            observed.add(name)
            if name in required_steps:
                if step.get("status") != "completed" or step.get("conclusion") != "success":
                    raise EvidenceError("required step missing completed/success status: " + name)
                job_successful_steps.add(name)
        absent = set(required_steps) - job_successful_steps
        if absent:
            raise EvidenceError("required executed steps absent from each job: " + ", ".join(sorted(absent)))
    return {"verdict": "executed_success", "head_sha": expected_sha,
            "run_id": expected_run_id, "attempt": expected_attempt,
            "jobs": len(jobs), "required_steps": sorted(successful_steps)}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-json", type=Path, required=True)
    p.add_argument("--jobs-json", type=Path, required=True)
    p.add_argument("--sha", required=True)
    p.add_argument("--run-id", type=int, required=True)
    p.add_argument("--attempt", type=int, required=True)
    p.add_argument("--expected-jobs", type=int, required=True)
    p.add_argument("--required-step", action="append", required=True)
    args = p.parse_args(argv)
    try:
        verdict = verify(json.loads(args.run_json.read_text(encoding="utf-8")),
                         json.loads(args.jobs_json.read_text(encoding="utf-8")),
                         expected_sha=args.sha, expected_run_id=args.run_id,
                         expected_attempt=args.attempt,
                         expected_job_count=args.expected_jobs,
                         required_steps=args.required_step)
    except (EvidenceError, ValueError, OSError) as exc:
        print(json.dumps({"verdict": "unverified", "reason": str(exc)}, sort_keys=True))
        return 1
    print(json.dumps(verdict, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
