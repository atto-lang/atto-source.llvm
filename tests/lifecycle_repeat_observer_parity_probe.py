#!/usr/bin/env python3
"""Source-level L01/L02 admission *and generated call-count* probe.

Diagnostic-only by default. --require-ir-admission fails if any target
rejects or fails to emit exactly one coalesced lifecycle bridge call. Even a passing
IR gate is not LLVM->Clang native guest execution or Java parity.
"""
import argparse
import pathlib
import re
import subprocess
import sys

# Never count declarations, declarations of @symbols, or comments as calls.
BRIDGE = {
    "source-actor-lifecycle-repeat-ready-target.ores":
        "oreslang_source_actor_await_ready",
    "source-actor-lifecycle-repeat-ready-token-target.ores":
        "oreslang_source_actor_await_ready",
    "source-actor-lifecycle-repeat-done-target.ores":
        "oreslang_source_actor_await_done",
}

def lifecycle_calls(ir, symbol):
    # LLVM prints a real call as "call i64 @..." (possibly with attributes).
    # Declarations, source comments and symbol-name text are not execution.
    pattern = re.compile(
        r"\bcall\s+(?:[A-Za-z_0-9]+\s+)+@" + re.escape(symbol) + r"\s*\("
    )
    return [line for line in ir.splitlines()
            if not line.lstrip().startswith(";") and pattern.search(line)]

def probe(compiler, path):
    return subprocess.run([compiler, str(path)], capture_output=True,
                          text=True, timeout=90, check=False)

def evaluate(name, result):
    if result.returncode != 0:
        if result.stdout.strip():
            raise RuntimeError(f"{name}: rejected compiler leaked partial LLVM IR")
        return f"OPEN_GAP {name} code={result.returncode} diagnostic={result.stderr.strip()[-250:]}", False
    if not result.stdout.startswith("; ModuleID"):
        raise RuntimeError(f"{name}: compiler succeeded without LLVM IR")
    calls = lifecycle_calls(result.stdout, BRIDGE[name])
    if len(calls) != 1:
        return f"OPEN_GAP {name} guest_bridge_calls={len(calls)} required=1", False
    return f"IR_ONLY_NEEDS_NATIVE_EXECUTION {name} coalesced_bridge_calls=1", True

def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--require-ir-admission", action="store_true")
    args = ap.parse_args(argv)
    base = pathlib.Path(__file__).resolve().parent / "fixtures"
    gaps = 0
    for name in BRIDGE:
        message, admitted = evaluate(name, probe(args.compiler, base / name))
        print(message, flush=True)
        gaps += int(not admitted)
    print(f"UNVERIFIED_SOURCE_PARITY gaps={gaps} total={len(BRIDGE)}")
    if args.require_ir_admission and gaps:
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
