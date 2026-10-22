#!/usr/bin/env python3
"""Fail closed on unsupported native reusable SelectPlan ownership syntax."""
import argparse
import pathlib
import subprocess
import sys

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixtures", required=True)
    args = parser.parse_args()
    for name in ("unsupported-select-plan-constructor.ores", "unsupported-select-case-write.ores"):
        fixture = pathlib.Path(args.fixtures) / name
        process = subprocess.run([args.compiler, str(fixture)], capture_output=True,
                                 text=True, check=False, timeout=20)
        if (process.returncode != 1 or process.stdout.strip()
                or "native SelectPlan/SelectCase ownership unsupported" not in process.stderr):
            raise AssertionError(f"{name}: native select descriptor must fail without IR: {process.stderr}")
    print("native linear SelectPlan admission: 2/2 rejected before LLVM IR")

if __name__ == "__main__":
    try:
        main()
    except (OSError, AssertionError, subprocess.TimeoutExpired) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
