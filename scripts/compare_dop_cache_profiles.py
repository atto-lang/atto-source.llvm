#!/usr/bin/env python3
"""Fail closed when comparing three single-layout Linux perf-stat DOP runs.

This compares counter evidence; it cannot prove CPU pinning, NUMA locality,
source-to-binary provenance, GPU execution, or performance improvement.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import statistics
import stat

from profile_cache_locality import EVENTS, rates, validate_event

SHA = re.compile(r"[0-9a-f]{40}\Z")
LAYOUTS = ("aos", "soa", "aosoa")
MAX_PROFILE_BYTES = 1024 * 1024


def read_bounded_report(path):
    """Read only a bounded regular evidence file, never a link or pipe.

    O_NONBLOCK prevents an attacker-controlled FIFO from hanging the reader.
    The open descriptor is fstat'd to avoid a path-check/read race.
    """
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NONBLOCK", 0)
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    fd = os.open(path, flags)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                "profile input must be a single-link regular file")
        payload = stream.read(MAX_PROFILE_BYTES + 1)
    require(0 < len(payload) <= MAX_PROFILE_BYTES,
            "profile input is empty or exceeds the 1 MiB evidence limit")
    return payload.decode("utf-8")



def require(condition, description):
    if not condition:
        raise ValueError(description)


def numeric(value, where):
    require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
            f"invalid numeric {where}")
    return value


def command_key(command, expected):
    # Match the native benchmark's exact seven-argv grammar. An evidence
    # comparator must not accept options the actual executable rejects.
    require(isinstance(command, list) and len(command) >= 2
            and all(isinstance(arg, str) for arg in command), "invalid command")
    require(Path(command[0]).name in ("native_actor_shard_layout_bench",
                                      "oreslang-native-actor-shard-bench"),
            "not the native actor shard layout benchmark")
    require(command.count("--layout") == 1 and command[1:3] == ["--layout", expected]
            and not any(arg.startswith("--layout=") for arg in command),
            "wrong, missing or duplicate benchmark layout")
    require(len(command) == 7 and command[3] == "--rows"
            and command[5] == "--iterations", "invalid benchmark workload arguments")
    numbers = []
    for index, name in ((4, "rows"), (6, "iterations")):
        value = command[index]
        require(0 < len(value) <= 20 and value.isascii() and value.isdecimal()
                and 1 <= int(value) <= 100000, f"invalid {name} workload")
        numbers.append(int(value))
    require(numbers[0] * numbers[1] <= 50000000,
            "benchmark workload exceeds 50 million row-updates")
    return [command[0], "--rows", command[4], "--iterations", command[6]]


def reject_duplicate_json_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key in profiler evidence: " + key)
        result[key] = value
    return result


def load_profile(path, layout):
    def reject_constant(token):
        raise ValueError("nonfinite JSON number: " + token)
    report = json.loads(read_bounded_report(path),
                        parse_constant=reject_constant,
                        object_pairs_hook=reject_duplicate_json_pairs)
    require(isinstance(report, dict) and report.get("format") == "oreslang-cpu-cache-v1",
            "unexpected profiler format")
    tree = report.get("source_tree")
    require(isinstance(tree, str) and SHA.fullmatch(tree), "missing exact source-tree identity")
    command = command_key(report.get("command"), layout)
    require(Path(command[0]).is_absolute(), "benchmark executable path must be absolute")
    binary_hash = report.get("binary_sha256")
    binary_size = report.get("binary_size_bytes")
    require(isinstance(binary_hash, str) and
            re.fullmatch(r"[0-9a-f]{64}", binary_hash) is not None,
            "missing or invalid binary SHA-256 evidence")
    require(type(binary_size) is int and 0 < binary_size <= 512 * 1024 * 1024,
            "missing or invalid binary size evidence")
    extras = report.get("hardware_l2_events")
    require(isinstance(extras, list) and len(extras) in (0, 2)
            and all(isinstance(e, str) for e in extras)
            and len(set(extras)) == len(extras), "invalid explicit L2 event pair")
    try:
        for event in extras:
            validate_event(event)
    except ValueError as exc:
        raise ValueError("invalid explicit L2 event pair: " + str(exc)) from exc
    events = list(EVENTS) + extras
    require(report.get("events") == events, "counter event set/order mismatch")
    host = report.get("host")
    require(isinstance(host, dict) and isinstance(host.get("machine"), str)
            and isinstance(host.get("kernel"), str)
            and isinstance(host.get("allowed_cpus"), list)
            and len(host["allowed_cpus"]) > 0
            and all(type(i) is int and i >= 0 for i in host["allowed_cpus"])
            and host["allowed_cpus"] == sorted(set(host["allowed_cpus"])),
            "missing/invalid CPU host/cpuset evidence")
    samples = report.get("samples")
    require(isinstance(samples, list) and 1 <= len(samples) <= 30
            and type(report.get("repeats")) is int and report["repeats"] == len(samples)
            and type(report.get("warmups")) is int and 0 <= report["warmups"] <= 20,
            "missing or incomplete perf samples")
    computed = []
    for i, sample in enumerate(samples):
        require(isinstance(sample, dict) and isinstance(sample.get("counters"), dict)
                and set(sample["counters"]) == set(events), "missing/extra counter")
        counters = sample["counters"]
        for name in events:
            numeric(counters[name], f"{layout}[{i}].{name}")
        require(counters["cycles"] > 0 and counters["instructions"] > 0,
                "zero cycle/instruction denominator")
        for total, missed in (("L1-dcache-loads", "L1-dcache-load-misses"),
                              ("LLC-loads", "LLC-load-misses")):
            require(counters[total] > 0 and counters[missed] <= counters[total],
                    "invalid cache miss denominator")
        observed = rates(counters, tuple(extras) if extras else None)
        provided = sample.get("rates")
        require(isinstance(provided, dict) and provided.keys() == observed.keys()
                and all(type(provided[k]) in (int, float)
                        and math.isfinite(provided[k])
                        and math.isclose(provided[k], v, rel_tol=1e-12, abs_tol=1e-12)
                        for k, v in observed.items()), "invalid or forged per-sample rates")
        computed.append(observed)
    median = {key: statistics.median(sample[key] for sample in computed)
              for key in computed[0]}
    provided = report.get("median_rates")
    require(isinstance(provided, dict) and provided.keys() == median.keys()
            and all(type(provided[key]) in (int, float)
                    and math.isfinite(provided[key])
                    and math.isclose(provided[key], val, rel_tol=1e-12, abs_tol=1e-12)
                    for key, val in median.items()), "invalid or forged medians")
    return report, command, median


def compare(paths):
    values = {layout: load_profile(paths[layout], layout) for layout in LAYOUTS}
    first, first_command, _ = values["aos"]
    for layout in LAYOUTS[1:]:
        report, command, _ = values[layout]
        for field in ("source_tree", "binary_sha256", "binary_size_bytes",
                      "host", "events", "hardware_l2_events",
                      "warmups", "repeats"):
            require(report[field] == first[field], f"incomparable {field} for {layout}")
        require(command == first_command, f"incomparable benchmark workload for {layout}")
    medians = {name: value[2] for name, value in values.items()}
    return {
        "format": "oreslang-cpu-cache-comparison-v1",
        "source_tree": first["source_tree"],
        "binary_sha256": first["binary_sha256"],
        "binary_size_bytes": first["binary_size_bytes"],
        "host": first["host"],
        "workload_except_layout": first_command,
        "median_rates": medians,
        "relative_to_aos": {
            name: {metric: (value / medians["aos"][metric]
                            if medians["aos"][metric] > 0 else None)
                   for metric, value in stats.items()}
            for name, stats in medians.items() if name != "aos"
        },
        "limitations": "Counter comparison and stable binary SHA only; no source-to-binary provenance, GPU or affinity proof",
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for layout in LAYOUTS:
        parser.add_argument("--" + layout, required=True)
    args = parser.parse_args(argv)
    try:
        print(json.dumps(compare({x: getattr(args, x) for x in LAYOUTS}),
                         indent=2, sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as exc:
        parser.exit(2, f"incomparable cache profiles: {exc}\n")


if __name__ == "__main__":
    main()
