#!/usr/bin/env python3
"""Fail-closed Linux perf cache-counter harness for one native DOP benchmark.

Host CPU counters only; not a GPU profiler or proof of actor/Task affinity.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

EVENTS = (
    "cycles", "instructions", "L1-dcache-loads", "L1-dcache-load-misses",
    "LLC-loads", "LLC-load-misses", "cpu-migrations", "context-switches",
)
EVENT_NAME = re.compile(r"[A-Za-z0-9_.:/=-]{1,100}\Z")


def parse_perf_stat(raw, required=EVENTS):
    """Extract an exact, nonduplicated event set from perf -x ';' output."""
    required = tuple(required)
    if len(set(required)) != len(required):
        raise ValueError("duplicate requested perf event")
    values = {}
    for line in raw.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split(";")]
        if len(parts) < 3:
            continue
        name = parts[2]
        if name not in required:
            continue
        if name in values:
            raise ValueError(f"duplicate counter: {name}")
        if not re.fullmatch(r"-?(?:[0-9]+(?:\.[0-9]+)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?", parts[0]):
            raise ValueError(f"missing, unsupported, or malformed counter: {name}")
        value = float(parts[0])
        if not math.isfinite(value) or value < 0:
            raise ValueError(f"invalid counter: {name}")
        # perf may multiplex CPU counters and scale different workload
        # phases independently; reject samples lacking full running coverage.
        if len(parts) <= 4 or not re.fullmatch(r"[0-9]+(?:\.[0-9]+)?", parts[4]):
            raise ValueError(f"missing perf time-running percentage: {name}")
        running = float(parts[4])
        if not 98.0 <= running <= 100.1:
            raise ValueError(f"multiplexed or invalid perf counter: {name}")
        values[name] = value
    missing = set(required) - values.keys()
    if missing:
        raise ValueError("missing perf events: " + ", ".join(sorted(missing)))
    if values["instructions"] <= 0 or values["cycles"] <= 0:
        raise ValueError("zero instruction/cycle counters cannot normalize workload")
    for loads, misses in (("L1-dcache-loads", "L1-dcache-load-misses"),
                           ("LLC-loads", "LLC-load-misses")):
        if values[loads] <= 0 or values[misses] > values[loads]:
            raise ValueError(f"invalid cache-load denominator: {loads}")
    return values


def rates(values, l2_pair=None):
    instructions = values["instructions"]
    output = {
        "ipc": instructions / values["cycles"],
        "l1d_miss_pct": 100 * values["L1-dcache-load-misses"] / values["L1-dcache-loads"],
        "llc_miss_pct": 100 * values["LLC-load-misses"] / values["LLC-loads"],
        "l1d_mpki": 1000 * values["L1-dcache-load-misses"] / instructions,
        "llc_mpki": 1000 * values["LLC-load-misses"] / instructions,
    }
    if l2_pair:
        requests, misses = l2_pair
        if values[requests] <= 0 or values[misses] > values[requests]:
            raise ValueError("invalid L2 event denominator")
        output["l2_miss_pct"] = 100 * values[misses] / values[requests]
        output["l2_mpki"] = 1000 * values[misses] / instructions
    return output


def validate_event(name):
    if not EVENT_NAME.fullmatch(name):
        raise ValueError("invalid perf event syntax")
    if name in EVENTS:
        raise ValueError("optional L2 event duplicates mandatory event")
    return name


def current_tree():
    proc = subprocess.run(["git", "rev-parse", "HEAD^{tree}"],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, check=True)
    return proc.stdout.strip()


def profile(args):
    if sys.platform != "linux":
        raise RuntimeError("Linux perf is required; no simulated cache numbers")
    if args.repeats < 1 or args.repeats > 30 or args.warmups < 0 or args.warmups > 20:
        raise ValueError("repeats must be 1..30 and warmups 0..20")
    if args.tree:
        if not re.fullmatch(r"[0-9a-f]{40}", args.tree):
            raise ValueError("tree identity must be a 40-character lowercase Git SHA-1")
        if current_tree() != args.tree:
            raise RuntimeError("worktree HEAD tree does not match --tree")
        status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=all", "--ignored"],
                                capture_output=True, text=True, check=True)
        if status.stdout.strip():
            raise RuntimeError("worktree is dirty; no exact-tree cache evidence")
    if not args.command:
        raise ValueError("missing benchmark command after --")
    if args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        raise ValueError("missing benchmark executable")
    perf = shutil.which(args.perf)
    if not perf:
        raise RuntimeError("perf executable unavailable")
    if bool(args.l2_requests) != bool(args.l2_misses):
        raise ValueError("L2 requires BOTH architecture-specific request and miss event names")
    extra = (validate_event(args.l2_requests), validate_event(args.l2_misses)) if args.l2_requests else ()
    if extra and extra[0] == extra[1]:
        raise ValueError("L2 request and miss events must differ")
    events = EVENTS + extra
    output = Path(args.output)
    if not output.parent.is_dir():
        raise RuntimeError("output directory must exist")
    if output.exists():
        raise RuntimeError("output file exists; refusing to overwrite previous evidence")
    for _ in range(args.warmups):
        subprocess.run(args.command, check=True, stdout=subprocess.DEVNULL)
    samples = []
    for _ in range(args.repeats):
        with tempfile.TemporaryDirectory(prefix="ores-cache-") as dirname:
            stat_file = Path(dirname) / "stat.txt"
            cmd = [perf, "stat", "-x", ";", "-e", ",".join(events), "-o",
                   str(stat_file), "--", *args.command]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            counters = parse_perf_stat(stat_file.read_text(encoding="utf-8"), events)
            samples.append({"counters": counters, "rates": rates(counters, extra or None)})
    keys = samples[0]["rates"]
    report = {
        "format": "oreslang-cpu-cache-v1",
        "source_tree": current_tree() if args.tree else None,
        "command": args.command,
        "events": list(events),
        "hardware_l2_events": list(extra),
        "repeats": args.repeats,
        "warmups": args.warmups,
        "host": {"machine": os.uname().machine, "kernel": os.uname().release,
                 "allowed_cpus": sorted(os.sched_getaffinity(0))},
        "median_rates": {key: statistics.median(row["rates"][key] for row in samples)
                         for key in keys},
        "samples": samples,
    }
    # A failed or unsupported counter must never leave a success artifact.
    with tempfile.NamedTemporaryFile(mode="w", dir=output.parent, prefix=".cache-",
                                     suffix=".json", delete=False, encoding="utf-8") as fp:
        tmpname = fp.name
        json.dump(report, fp, indent=2, sort_keys=True)
        fp.write("\n")
    try:
        os.replace(tmpname, output)
    finally:
        if os.path.exists(tmpname):
            os.unlink(tmpname)
    print(output)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, help="JSON output; written only after complete success")
    parser.add_argument("--perf", default="perf")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--tree", help="require exact Git HEAD tree and clean worktree")
    parser.add_argument("--l2-requests", help="architecture-specific L2 request event")
    parser.add_argument("--l2-misses", help="architecture-specific L2 miss event")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    try:
        profile(args)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        parser.exit(2, f"cache profiling failed (no result published): {exc}\n")


if __name__ == "__main__":
    main()
