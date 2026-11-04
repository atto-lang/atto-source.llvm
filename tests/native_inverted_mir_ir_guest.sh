#!/usr/bin/env bash
# Integration check for true generated IR + native Clang, no Python required.
set -euo pipefail
emitter="$1"; clang="$2"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
"$emitter" positive "$scratch/positive.ll"
for guard in 'icmp ult i64' 'icmp ne i64' 'load i1, ptr %alive' 'call void @llvm.trap()' 'store volatile i1 false, ptr %alive'; do
 if ! grep -Fq "$guard" "$scratch/positive.ll"; then
   echo "FAIL: checked MIR IR missing guard $guard"; exit 1
 fi
done
"$clang" -x ir "$scratch/positive.ll" -o "$scratch/positive"
set +e
"$scratch/positive" >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" -ne 42 ]; then echo "FAIL: positive MIR native exit: $rc"; exit 1; fi
"$emitter" closed "$scratch/closed.ll"
for guard in 'store volatile i1 false, ptr %alive' 'store volatile i64 0, ptr'; do
 if ! grep -Fq "$guard" "$scratch/closed.ll"; then
   echo "FAIL: typed MIR close omitted volatile erasure: $guard"; exit 1
 fi
done
# Every one of three 2-slot column/metadata lanes must be wiped.
wipe_count="$(grep -Fc 'store volatile i64 0, ptr' "$scratch/closed.ll")"
if [ "$wipe_count" -ne 7 ]; then
 echo "FAIL: expected 6 volatile column scrubs + length, got $wipe_count"; exit 1
fi
"$clang" -O2 -x ir "$scratch/closed.ll" -o "$scratch/closed"
set +e
"$scratch/closed" >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" -ne 42 ]; then echo "FAIL: closed MIR native exit: $rc"; exit 1; fi
"$emitter" bounds "$scratch/bounds.ll"
for guard in 'icmp ult i64' 'call void @llvm.trap()'; do
 if ! grep -Fq "$guard" "$scratch/bounds.ll"; then
   echo "FAIL: bounds MIR omitted guard $guard"; exit 1
 fi
done
"$clang" -x ir "$scratch/bounds.ll" -o "$scratch/bounds"
# A shell status of 132/133 is ambiguous: a program can return that exit code
# without receiving SIGILL/SIGTRAP. Python retains the actual signal number.
python3 - "$scratch/bounds" <<'PY'
import resource
import signal
import subprocess
import sys

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
try:
    result = subprocess.run(
        [sys.argv[1]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        timeout=15, check=False
    )
except (OSError, subprocess.TimeoutExpired) as exc:
    raise SystemExit(f"FAIL: bounds guest did not execute: {exc}")
if result.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
    raise SystemExit(
        f"FAIL: bounds guest expected a real SIGILL/SIGTRAP, got {result.returncode}"
    )
PY
for rejected in immutable missing-operand unclosed; do
 if "$emitter" "$rejected" "$scratch/$rejected.ll" >/dev/null 2>&1; then
   echo "FAIL: MIR compiler admitted $rejected"; exit 1
 fi
 if [ -s "$scratch/$rejected.ll" ]; then echo "FAIL: emitted unsafe $rejected IR"; exit 1; fi
done
printf 'PASS: strict C++17 typed MIR -> real native LLVM IR (exit 42, owner close erasure, bounds trap, three rejects)\n'
