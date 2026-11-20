#!/usr/bin/env python3
"""Fail-closed Linux perf cache-counter harness for one native DOP benchmark.

Host CPU counters only; not a GPU profiler or proof of actor/Task affinity.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import stat
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


def verify_exact_source_tree(expected):
    """Require a fixed tracked tree and no uncommitted/nonignored source files.

    Ignored build outputs are allowed: they are not part of the source tree
    and the independently hashed benchmark executable binds their bytes.
    """
    if not re.fullmatch(r"[0-9a-f]{40}", expected):
        raise ValueError("tree identity must be a 40-character lowercase Git SHA-1")
    if current_tree() != expected:
        raise RuntimeError("worktree HEAD tree does not match --tree")
    status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=all"],
                            capture_output=True, text=True, check=True)
    if status.stdout.strip():
        raise RuntimeError("worktree has uncommitted or untracked source; no exact-tree cache evidence")


def hash_executable(path):
    """Hash one executable through a no-follow descriptor, not a racy pathname."""
    if not path.is_absolute():
        raise ValueError("benchmark executable path must be absolute")
    if not hasattr(os, "O_NOFOLLOW"):
        raise RuntimeError("platform cannot safely bind benchmark executable identity")
    flags = os.O_RDONLY | os.O_NOFOLLOW | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NONBLOCK", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise ValueError("benchmark executable path must be a regular file, not a symlink") from exc
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1
                or info.st_size <= 0 or info.st_size > 512 * 1024 * 1024):
            raise ValueError("benchmark executable must be a single-link regular file of 1..512 MiB")
        if info.st_mode & 0o111 == 0:
            raise ValueError("benchmark binary must be executable")
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        again = os.fstat(stream.fileno())
        if (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns) != (
                again.st_dev, again.st_ino, again.st_size, again.st_mtime_ns, again.st_ctime_ns):
            raise RuntimeError("benchmark executable changed while being hashed")
    return digest.hexdigest(), info.st_size


def publish_exclusive(temp_path, destination):
    """Atomically publish only if the destination has no directory entry.

    os.replace would overwrite a report or symlink created after the
    preflight exists() check. The temporary and final paths share a directory.
    """
    try:
        os.link(temp_path, destination, follow_symlinks=False)
    except FileExistsError as exc:
        raise RuntimeError("output appeared during profiling; refusing overwrite") from exc


def require_stable_cpu_mask(expected):
    """Reject drift in the parent's effective CPU mask at run boundaries.

    This does not prove child-thread pinning or detect a mask changed and
    restored between checks; actual residency still needs OS-level evidence.
    """
    observed = sorted(os.sched_getaffinity(0))
    if not observed or observed != expected:
        raise RuntimeError("effective CPU mask changed during cache profiling")


def profile(args):
    if sys.platform != "linux":
        raise RuntimeError("Linux perf is required; no simulated cache numbers")
    if args.repeats < 1 or args.repeats > 30 or args.warmups < 0 or args.warmups > 20:
        raise ValueError("repeats must be 1..30 and warmups 0..20")
    allowed_cpus = sorted(os.sched_getaffinity(0))
    if not allowed_cpus:
        raise RuntimeError("empty effective CPU mask; refusing cache profiling")
    if args.tree:
        verify_exact_source_tree(args.tree)
    if not args.command:
        raise ValueError("missing benchmark command after --")
    if args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        raise ValueError("missing benchmark executable")
    binary = Path(args.command[0])
    binary_sha256, binary_size = hash_executable(binary)
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
        require_stable_cpu_mask(allowed_cpus)
        subprocess.run(args.command, check=True, stdout=subprocess.DEVNULL)
        require_stable_cpu_mask(allowed_cpus)
    samples = []
    for _ in range(args.repeats):
        require_stable_cpu_mask(allowed_cpus)
        with tempfile.TemporaryDirectory(prefix="ores-cache-") as dirname:
            stat_file = Path(dirname) / "stat.txt"
            cmd = [perf, "stat", "-x", ";", "-e", ",".join(events), "-o",
                   str(stat_file), "--", *args.command]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            require_stable_cpu_mask(allowed_cpus)
            counters = parse_perf_stat(stat_file.read_text(encoding="utf-8"), events)
            samples.append({"counters": counters, "rates": rates(counters, extra or None)})
    # Refuse to publish measurements after CPU-mask drift or binary rewrite.
    require_stable_cpu_mask(allowed_cpus)
    if hash_executable(binary) != (binary_sha256, binary_size):
        raise RuntimeError("benchmark executable changed during profiling")
    if args.tree:
        verify_exact_source_tree(args.tree)
    keys = samples[0]["rates"]
    report = {
        "format": "oreslang-cpu-cache-v1",
        "source_tree": current_tree() if args.tree else None,
        "binary_sha256": binary_sha256,
        "binary_size_bytes": binary_size,
        "command": args.command,
        "events": list(events),
        "hardware_l2_events": list(extra),
        "repeats": args.repeats,
        "warmups": args.warmups,
        "host": {"machine": os.uname().machine, "kernel": os.uname().release,
                 "allowed_cpus": allowed_cpus},
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
        publish_exclusive(tmpname, output)
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
