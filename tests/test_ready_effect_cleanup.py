#!/usr/bin/env python3
"""Fail closed if readiness-effect OS failure bypasses host record/string cleanup."""
import argparse
import re
import subprocess
import sys


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()

    result = subprocess.run(
        [args.compiler, args.fixture],
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )
    require(result.returncode == 0,
            f"compiler failed with exit code {result.returncode}: {result.stderr}")
    require(not result.stderr, "compiler emitted unexpected stderr")
    ir = result.stdout
    require(ir.startswith("; ModuleID"), "compiler output is not LLVM IR")

    match = re.search(r"(?m)^define\s+i32\s+@main\s*\(\s*\)\s*\{", ir)
    require(match is not None, "host i32 main definition missing")
    start = match.start()
    next_function = re.search(r"(?m)^define\s+", ir[match.end():])
    end = match.end() + next_function.start() if next_function else len(ir)
    host = ir[start:end]

    require("@ores_record_region_begin" in host,
            "fixture did not activate host record runtime")
    require("@ores_string_region_begin" in host,
            "fixture did not activate host string runtime")
    throw = host.find("main.ready.throw:")
    returned = host.find("main.ready.return:")
    require(throw >= 0 and returned > throw,
            "readiness throw/return blocks missing or reordered")
    block = host[throw:returned]
    record_cleanup = block.find("@ores_record_region_end")
    string_cleanup = block.find("@ores_string_region_end")
    report = block.find("@oreslang_source_actor_unhandled_ready_effect")
    require(record_cleanup >= 0,
            "readiness throw block bypasses record-region cleanup")
    require(string_cleanup >= 0,
            "readiness throw block bypasses string-region cleanup")
    require(block.count("@ores_record_region_end") == 1,
            "readiness throw block closes record region more than once")
    require(block.count("@ores_string_region_end") == 1,
            "readiness throw block closes string region more than once")
    require(report >= 0, "readiness throw block does not report ActorTerminated")
    require(record_cleanup < string_cleanup < report,
            "readiness failure cleanup order must be record then string then report")
    require(block.find("ret i32") > report,
            "readiness throw block returns before effect reporting")

    print("READY_EFFECT_HOST_CLEANUP_PASS order=record_end-before-string_end-before-report")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"ready effect cleanup test failed: {error}", file=sys.stderr)
        sys.exit(1)
