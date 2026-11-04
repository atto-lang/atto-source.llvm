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
# Detached values must survive owner writes, swap-delete and volatile close,
# including reads of an immutable column. Compile the actual generated IR
# under both optimization modes rather than accepting metadata-only evidence.
for mode in snapshot snapshot-id; do
 "$emitter" "$mode" "$scratch/$mode.ll"
 for guard in 'icmp ult i64' 'icmp ne i64' 'store volatile i1 false, ptr %alive' 'store i64 99, ptr'; do
  if ! grep -Fq "$guard" "$scratch/$mode.ll"; then
   echo "FAIL: $mode missing detached SSA/owner guard: $guard"; exit 1
  fi
 done
 for opt in -O0 -O2; do
  "$clang" "$opt" -x ir "$scratch/$mode.ll" -o "$scratch/$mode$opt"
  set +e
  "$scratch/$mode$opt" >/dev/null 2>&1
  rc=$?
  set -e
  if [ "$rc" -ne 42 ]; then echo "FAIL: $mode $opt returned $rc, expected detached 42"; exit 1; fi
 done
done
for bad in bounds snapshot-bounds; do
 "$emitter" "$bad" "$scratch/$bad.ll"
 for guard in 'icmp ult i64' 'call void @llvm.trap()'; do
  if ! grep -Fq "$guard" "$scratch/$bad.ll"; then
   echo "FAIL: $bad omitted guard $guard"; exit 1
  fi
 done
 "$clang" -x ir "$scratch/$bad.ll" -o "$scratch/$bad"
# A shell status of 132/133 is ambiguous: a program can return that exit code
# without receiving SIGILL/SIGTRAP. Python retains the actual signal number.
python3 - "$scratch/$bad" <<'PY'
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
done
for rejected in immutable missing-operand unclosed invalid-snapshot; do
 if "$emitter" "$rejected" "$scratch/$rejected.ll" >"$scratch/$rejected.out" 2>"$scratch/$rejected.err"; then
   echo "FAIL: MIR compiler admitted $rejected"; exit 1
 fi
 if [ -s "$scratch/$rejected.ll" ] || [ -s "$scratch/$rejected.out" ]; then
   echo "FAIL: emitted unsafe $rejected IR"; exit 1
 fi
 if [ "$rejected" = invalid-snapshot ] && ! grep -Fq 'invalid inverted snapshot value reference' "$scratch/$rejected.err"; then
   echo "FAIL: missing typed snapshot-reference diagnostic"; exit 1
 fi
done
printf 'PASS: strict C++17 typed MIR -> native LLVM IR (detached snapshots O0/O2 exit 42, volatile close, two bounds traps, four rejects)\n'
