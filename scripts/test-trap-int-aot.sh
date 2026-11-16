#!/usr/bin/env bash
# Executed compiler -> no-GC IR auditor -> Clang -> guest proof.
# Not an ordinary-throw or nested Result parity claim.
set -euo pipefail
if [[ "$#" != 6 ]]; then
  echo "usage: $0 compiler clang auditor success.atto overflow.atto aot-driver" >&2
  exit 64
fi
compiler="$1"; clang="$2"; auditor="$3"; good="$4"; overflow="$5"; driver="$6"
for required in "$compiler" "$auditor" "$good" "$overflow" "$driver"; do
  [[ -f "$required" ]] || { echo "missing native input: $required" >&2; exit 1; }
done
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
ulimit -c 0 || true
for opt in 0 2; do
  export ORESLANG_LLVMC="$compiler"
  export ORES_NO_GC_IR_AUDIT="$auditor"
  export CLANG="$clang"
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  bash "$driver" "$good" "$work/success-$opt" >"$work/build-$opt.log" 2>&1 ||
    { cat "$work/build-$opt.log" >&2; exit 1; }
  set +e
  "$work/success-$opt" >"$work/stdout-$opt" 2>"$work/stderr-$opt"
  status=$?
  set -e
  if [[ "$status" != 42 || -s "$work/stdout-$opt" || -s "$work/stderr-$opt" ]]; then
    echo "real native trap Option<int> guest mismatch at O$opt (exit $status)" >&2
    cat "$work/stdout-$opt" "$work/stderr-$opt" >&2
    exit 1
  fi
  bash "$driver" "$overflow" "$work/overflow-$opt" >"$work/overflow-build-$opt.log" 2>&1 ||
    { cat "$work/overflow-build-$opt.log" >&2; exit 1; }
  set +e
  ( "$work/overflow-$opt" >"$work/overflow-stdout-$opt" 2>"$work/overflow-stderr-$opt" ) 2>/dev/null
  panic_status=$?
  set -e
  # llvm.trap must terminate with a signal, not merely return an error code.
  if [[ "$panic_status" != 132 && "$panic_status" != 133 ]]; then
    echo "checked overflow was swallowed or did not signal at O$opt (exit $panic_status)" >&2
    exit 1
  fi
done
echo "LLVM_NATIVE_TRAP_INT_COMPILED_GUEST=O0_O2_SUCCESS_AND_TERMINAL_OVERFLOW"
