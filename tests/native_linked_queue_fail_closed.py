#!/usr/bin/env python3
"""Fail-closed native admission for exact Java-tested queue sources.

All six exact-source fixtures remain unsupported in native LLVM: this test
MUST change to compiled-IR + executable checks when array/class support lands.
"""
import argparse
from pathlib import Path
import subprocess
import sys


FIXTURES = (
    "src/int_linked_queue.ores",
    "tests/smoke.ores",
    "tests/stress.ores",
    "tests/edge_cases.ores",
    "tests/instances.ores",
    "tests/relink_matrix.ores",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--fixture-root", required=True, type=Path)
    args = parser.parse_args()
    if not args.compiler.is_file():
        raise RuntimeError("native compiler executable is missing")
    for relative in FIXTURES:
        source = args.fixture_root / relative
        if not source.is_file() or source.stat().st_size == 0:
            raise RuntimeError("missing/empty exact-source fixture: " + str(source))
        result = subprocess.run(
            [str(args.compiler.resolve()), str(source.resolve())],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        if result.returncode == 0:
            raise AssertionError(
                f"{relative}: unlowered class/Array source was accepted")
        if result.stdout.strip():
            raise AssertionError(
                f"{relative}: partial LLVM IR/other stdout escaped rejection")
        if "oreslang-llvmc:" not in result.stderr:
            raise AssertionError(
                f"{relative}: expected compiler diagnostic, got: {result.stderr!r}")
        if "cannot open Oreslang source" in result.stderr:
            raise AssertionError(f"{relative}: source I/O failure masked admission")
        print(f"rejected without LLVM IR: {relative}", flush=True)
    print("PASS: six exact-source native queue inputs fail closed", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("native linked-queue gate failed:", exc, file=sys.stderr)
        sys.exit(1)
