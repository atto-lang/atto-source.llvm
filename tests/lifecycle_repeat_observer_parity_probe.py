#!/usr/bin/env python3
"""Diagnostic-only probe for L01/L02 source parity. Not a runtime pass gate.

A rejected guest source is an OPEN GAP, never a successful negative test.
Accepted LLVM IR is likewise not Clang execution or an accepted parity receipt.
"""
import argparse
import pathlib
import subprocess
import sys

FIXTURES = (
    "source-actor-lifecycle-repeat-ready-target.ores",
    "source-actor-lifecycle-repeat-ready-token-target.ores",
    "source-actor-lifecycle-repeat-done-target.ores",
)

def probe(compiler, path):
    return subprocess.run([compiler, str(path)], capture_output=True,
                          text=True, timeout=90, check=False)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--require-ir-admission", action="store_true")
    args = ap.parse_args()
    base = pathlib.Path(__file__).resolve().parent / "fixtures"
    gaps = []
    for name in FIXTURES:
        result = probe(args.compiler, base / name)
        if result.returncode != 0:
            if result.stdout.strip():
                raise RuntimeError(f"{name}: rejected compiler leaked partial LLVM IR")
            gaps.append(name)
            print(f"OPEN_GAP {name} returncode={result.returncode} diagnostic={result.stderr.strip()[-250:]}", flush=True)
            continue
        if not result.stdout.startswith("; ModuleID"):
            raise RuntimeError(f"{name}: compiler succeeded without LLVM IR")
        print(f"IR_ONLY_NEEDS_NATIVE_EXECUTION {name}", flush=True)
    print(f"UNVERIFIED_SOURCE_PARITY gaps={len(gaps)} total={len(FIXTURES)}")
    if args.require_ir_admission and gaps:
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
