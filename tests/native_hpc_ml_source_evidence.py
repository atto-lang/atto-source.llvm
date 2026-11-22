#!/usr/bin/env python3
"""Source-derived scalar bootstrap evidence. Never a distributed ML or GPU benchmark."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import statistics
import subprocess
import sys
import tempfile
import time


def checked_run(argv, *, timeout=30, stdin=None):
    return subprocess.run(argv, input=stdin, text=True, capture_output=True,
                          check=False, timeout=timeout)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def profile(args):
    compiler = Path(args.compiler).resolve(strict=True)
    clang = Path(args.clang).resolve(strict=True)
    fixture = Path(args.fixture).resolve(strict=True)
    if fixture.suffix != ".ores":
        raise ValueError("only actual .ores source fixtures may be profiled")
    if not compiler.is_file() or not clang.is_file() or not fixture.is_file():
        raise ValueError("compiler, clang and .ores source must be ordinary files")
    if args.samples < 1 or args.samples > 100 or args.warmups < 0 or args.warmups > 100:
        raise ValueError("sample count must be 1..100 and warmups 0..100")
    if not 0 <= args.expected_exit <= 255:
        raise ValueError("expected native exit code must fit the OS exit status")

    llvm = checked_run([str(compiler), str(fixture)])
    if llvm.returncode or llvm.stderr or not llvm.stdout.startswith("; ModuleID"):
        raise RuntimeError("source compiler failed closed: " + llvm.stderr[:1000])
    if "define i32 @main()" not in llvm.stdout:
        raise RuntimeError("source did not produce the native OS main wrapper")
    toolchain = checked_run([str(clang), "--version"])
    if toolchain.returncode:
        raise RuntimeError("clang version could not be determined")
    with tempfile.TemporaryDirectory(prefix="oreslang-ml-evidence-") as tmp:
        executable = Path(tmp) / "guest"
        link = checked_run([str(clang), "-x", "ir", "-", "-o", str(executable)],
                           stdin=llvm.stdout)
        if link.returncode:
            raise RuntimeError("LLVM IR could not link: " + link.stderr[:1000])

        def execute():
            before = resource.getrusage(resource.RUSAGE_CHILDREN)
            start = time.perf_counter_ns()
            result = checked_run([str(executable)], timeout=10)
            elapsed = (time.perf_counter_ns() - start) / 1_000_000.0
            after = resource.getrusage(resource.RUSAGE_CHILDREN)
            if result.returncode != args.expected_exit:
                raise RuntimeError("native program exit mismatch: " + str(result.returncode))
            return {"wall_ms": elapsed,
                    "user_cpu_s": after.ru_utime - before.ru_utime,
                    "system_cpu_s": after.ru_stime - before.ru_stime,
                    "minor_faults": after.ru_minflt - before.ru_minflt,
                    "major_faults": after.ru_majflt - before.ru_majflt}

        for _ in range(args.warmups):
            execute()
        samples = [execute() for _ in range(args.samples)]
        timings = sorted(x["wall_ms"] for x in samples)
        result = {
            "schema": "oreslang.native-source-scalar-evidence.v1",
            "workload_class": "scalar-bootstrap-only",
            "feature_claims": {"native_ores_source": True,
                               "distributed_ml": False, "source_actors": False,
                               "gpu": False, "no_gc_full_pipeline": False,
                               "java_parity": False},
            "source": {"path": str(fixture), "sha256": sha256(fixture)},
            "compiler": {"path": str(compiler), "sha256": sha256(compiler)},
            "clang": {"path": str(clang), "sha256": sha256(clang),
                      "version": toolchain.stdout.splitlines()[0]},
            "linked_binary_sha256": sha256(executable),
            "host": {"system": platform.system(), "machine": platform.machine(),
                     "cpu_count": os.cpu_count()},
            "samples": samples,
            "warmups": args.warmups,
            "p50_wall_ms": statistics.median(timings),
            "p95_wall_ms": timings[min(len(timings) - 1,
                                      max(0, (95 * len(timings) + 99) // 100 - 1))],
            "unavailable": ["GPU utilization", "ML inference throughput",
                            "actor queue latency", "network copies",
                            "per-process peak RSS", "GC/JIT full-pipeline proof",
                            "mixed-backend failover"]
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--expected-exit", type=int, default=42)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    args = parser.parse_args()
    try:
        print(json.dumps(profile(args), sort_keys=True))
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print("source evidence rejected: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
