#!/usr/bin/env bash
# Integration check for true generated IR + native Clang, no Python required.
set -euo pipefail
emitter="$1"; clang="$2"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
"$emitter" positive "$scratch/positive.ll"
"$clang" -x ir "$scratch/positive.ll" -o "$scratch/positive"
set +e
"$scratch/positive" >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" -ne 42 ]; then echo "FAIL: positive MIR native exit: $rc"; exit 1; fi
"$emitter" bounds "$scratch/bounds.ll"
"$clang" -x ir "$scratch/bounds.ll" -o "$scratch/bounds"
set +e
(ulimit -c 0; "$scratch/bounds" >/dev/null 2>&1) >/dev/null 2>&1
rc=$?
set -e
if [ "$rc" -eq 0 ] || [ "$rc" -eq 42 ]; then echo "FAIL: out-of-bounds MIR did not trap: $rc"; exit 1; fi
for rejected in immutable missing-operand; do
 if "$emitter" "$rejected" "$scratch/$rejected.ll" >/dev/null 2>&1; then
   echo "FAIL: MIR compiler admitted $rejected"; exit 1
 fi
 if [ -s "$scratch/$rejected.ll" ]; then echo "FAIL: emitted unsafe $rejected IR"; exit 1; fi
done
printf 'PASS: strict C++17 typed MIR -> real native LLVM IR (exit 42, bounds trap, two rejects)\n'
