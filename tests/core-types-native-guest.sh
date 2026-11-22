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
for f in edge-positive.atto edge-matrix-positive.atto void-value-negative.atto void-return-negative.atto bool-truthiness-negative.atto i64-literal-overflow-negative.atto wrong-arg-negative.atto u64-outside-int-negative.atto namespace-duplicate-negative.atto namespace-late-negative.atto; do
  [[ -f "$fixtures/$f" ]] || fail "missing $f"
done
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
for positive_fixture in edge-positive.atto edge-matrix-positive.atto; do
  "$compiler" --target=host "$fixtures/$positive_fixture" > "$work/positive.ll" 2> "$work/compiler.err" ||
    fail "$positive_fixture: real compiler could not link core/types"
  [[ -s "$work/positive.ll" ]] || fail "$positive_fixture: compiler produced no IR"
  grep -Fq '@main(' "$work/positive.ll" || fail "$positive_fixture: no compiled main in IR"
  for opt in 0 2; do
    "$clang" "-O$opt" -x ir "$work/positive.ll" -o "$work/positive-$opt" ||
      fail "$positive_fixture: LLVM->native link failure at O$opt"
    rc=0
    "$work/positive-$opt" > "$work/positive.stdout" 2> "$work/positive.stderr" || rc=$?
    [[ "$rc" -eq 0 ]] || fail "$positive_fixture: O$opt guest exited $rc instead of 0"
    [[ ! -s "$work/positive.stdout" && ! -s "$work/positive.stderr" ]] ||
      fail "$positive_fixture: O$opt unexpected guest output"
    echo "CORE_TYPES_LLVM_GUEST=$positive_fixture:O$opt:EXECUTED_EXIT_0"
  done
done
for f in void-value-negative.atto void-return-negative.atto bool-truthiness-negative.atto i64-literal-overflow-negative.atto wrong-arg-negative.atto u64-outside-int-negative.atto namespace-duplicate-negative.atto namespace-late-negative.atto; do
  rm -f -- "$work/rejected.ll"
  if "$compiler" --target=host "$fixtures/$f" > "$work/rejected.ll" 2> "$work/negative.err"; then
    fail "$f unexpectedly compiled"
  fi
  [[ ! -s "$work/rejected.ll" ]] || fail "$f left partial LLVM IR"
  [[ -s "$work/negative.err" ]] || fail "$f had no compiler diagnostic"
  # A generic nonzero exit is not an admission proof. Require fixture-
  # appropriate source diagnostics, especially for header-order errors.
  case "$f" in
    namespace-late-negative.atto)
      grep -Fq 'NAMESPACE_HEADER_ORDER' "$work/negative.err" ||
        fail "$f did not reject the post-import namespace header"
      ;;
    namespace-duplicate-negative.atto)
      grep -Fq 'NAMESPACE_DUPLICATE' "$work/negative.err" ||
        fail "$f did not reject the duplicate namespace header"
      ;;
    wrong-arg-negative.atto)
      grep -Eiq 'type|argument|int|bool|mismatch' "$work/negative.err" ||
        fail "$f did not diagnose the bool-to-int argument"
      ;;
    u64-outside-int-negative.atto|i64-literal-overflow-negative.atto)
      grep -Eiq 'range|overflow|literal|integer|int64|int' "$work/negative.err" ||
        fail "$f did not diagnose the integer literal outside the int domain"
      ;;
    *)
      grep -Eiq 'error|invalid|unsupported|type|void|bool|int|expected' "$work/negative.err" ||
        fail "$f did not produce a recognized compile-time error"
      ;;
  esac
  echo "CORE_TYPES_LLVM_COMPILE_REJECTED=$f"
done
echo "CORE_TYPES_LLVM_GUEST=4_EXECUTED_8_REJECTED"
