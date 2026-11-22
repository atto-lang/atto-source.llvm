#!/usr/bin/env bash
# Real Atto compiler -> LLVM IR -> Clang O0/O2 -> executed guest.
# Compiling source without running the guest is NOT a pass.
set -euo pipefail
if (( $# != 3 )); then
  echo "usage: $0 ORESLANG_LLVMC CLANG FIXTURE_DIR" >&2
  exit 64
fi
compiler="$1"; clang="$2"; fixtures="$3"
fail(){ echo "CORE_TYPES_LLVM_GUEST_FAIL: $*" >&2; exit 1; }
[[ -x "$compiler" ]] || fail "missing executable Atto compiler"
command -v "$clang" >/dev/null 2>&1 || fail "missing Clang"
for f in edge-positive.atto void-value-negative.atto void-return-negative.atto bool-truthiness-negative.atto i64-literal-overflow-negative.atto; do
  [[ -f "$fixtures/$f" ]] || fail "missing $f"
done
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$fixtures/edge-positive.atto" > "$work/positive.ll" 2> "$work/compiler.err" ||
  fail "real compiler could not link core/types into positive fixture"
[[ -s "$work/positive.ll" ]] || fail "positive compiler produced no IR"
grep -Fq '@main(' "$work/positive.ll" || fail "missing compiled main in IR"
for opt in 0 2; do
  "$clang" "-O$opt" -x ir "$work/positive.ll" -o "$work/positive-$opt" ||
    fail "failed LLVM->native link at O$opt"
  rc=0
  "$work/positive-$opt" > "$work/positive.stdout" 2> "$work/positive.stderr" || rc=$?
  [[ "$rc" -eq 0 ]] || fail "O$opt guest exit $rc, expected zero"
  [[ ! -s "$work/positive.stdout" && ! -s "$work/positive.stderr" ]] ||
    fail "O$opt unexpected guest stdout/stderr"
  echo "CORE_TYPES_LLVM_COMPILED_GUEST_O$opt=EXECUTED_EXIT_0"
done
for f in void-value-negative.atto void-return-negative.atto bool-truthiness-negative.atto i64-literal-overflow-negative.atto; do
  rm -f -- "$work/rejected.ll"
  if "$compiler" --target=host "$fixtures/$f" > "$work/rejected.ll" 2> "$work/negative.err"; then
    fail "$f unexpectedly compiled"
  fi
  [[ ! -s "$work/rejected.ll" ]] || fail "$f left partial LLVM IR"
  [[ -s "$work/negative.err" ]] || fail "$f had no compiler diagnostic"
  grep -Eiq 'error|invalid|unsupported|type|range|overflow' "$work/negative.err" ||
    fail "$f failed without a compiler-owned diagnostic"
  echo "CORE_TYPES_LLVM_COMPILE_REJECTED=$f"
done
echo "CORE_TYPES_LLVM_GUEST=2_EXECUTED_4_REJECTED"
