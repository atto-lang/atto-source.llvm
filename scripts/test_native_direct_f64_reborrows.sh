#!/usr/bin/env bash
set -euo pipefail
umask 077
root="$(cd "$(dirname "$0")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
compiler="$(printenv ORESLANG_LLVMC || true)"
auditor="$(printenv ORES_NO_GC_IR_AUDIT || true)"
driver="$(printenv ATTO_AOT_DRIVER || true)"
clang="$(printenv CLANG || true)"
if [[ -z "$compiler" || -z "$auditor" || -z "$driver" || -z "$clang" ||
      ! -x "$compiler" || ! -x "$auditor" || ! -f "$driver" ]] ||
      ! command -v "$clang" >/dev/null 2>&1; then
  echo "NOT_EXECUTED: require pinned compiler, IR auditor, AOT driver and CLANG" >&2
  exit 2
fi
for case_name in mixed-loan shared-escalation; do
  case "$case_name" in
    mixed-loan) expected="conflicting active native borrow";;
    shared-escalation) expected="cannot mutably reborrow shared parameter";;
  esac
  if "$compiler" --target=host "$root/tests/native-direct-array-f64/$case_name.atto" >"$tmp/rejected.ll" 2>"$tmp/rejected.err"; then
    echo "UNSAFE_DIRECT_F64_REBORROW_ADMITTED=$case_name" >&2
    exit 1
  fi
  [[ ! -s "$tmp/rejected.ll" && -s "$tmp/rejected.err" ]] || {
    echo "DIRECT_F64_REJECTION_NOT_FAIL_CLOSED=$case_name" >&2
    exit 1
  }
  grep -Fq "oreslang-llvmc: $expected" "$tmp/rejected.err" || {
    cat "$tmp/rejected.err" >&2
    echo "DIRECT_F64_REJECTION_WRONG_DIAGNOSTIC=$case_name" >&2
    exit 1
  }
  echo "DIRECT_F64_REBORROW_NEGATIVE_NO_IR=$case_name"
done
for opt in 0 2; do
  export ORESLANG_LLVMC="$compiler" ORES_NO_GC_IR_AUDIT="$auditor" CLANG="$clang"
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  if ! bash "$driver" "$root/tests/native-direct-array-f64/nested-reborrow.atto" "$tmp/guest-O$opt" >"$tmp/build.out" 2>"$tmp/build.err"; then
    cat "$tmp/build.out" "$tmp/build.err" >&2
    echo "DIRECT_F64_NESTED_BUILD_FAILED=O$opt" >&2
    exit 1
  fi
  [[ "$(head -c 4 "$tmp/guest-O$opt" | od -An -tx1 | tr -d ' \n')" == "7f454c46" ]] || {
    echo "DIRECT_F64_NESTED_NOT_ELF=O$opt" >&2
    exit 1
  }
  set +e
  timeout 15s "$tmp/guest-O$opt" >"$tmp/guest.out" 2>"$tmp/guest.err"
  status=$?
  set -e
  [[ "$status" -eq 42 && ! -s "$tmp/guest.out" && ! -s "$tmp/guest.err" ]] || {
    echo "DIRECT_F64_NESTED_GUEST_FAILED=O$opt;exit=$status" >&2
    cat "$tmp/guest.out" "$tmp/guest.err" >&2
    exit 1
  }
  echo "DIRECT_F64_NESTED_REBORROW_NATIVE_PASS=O$opt;sha256=$(sha256sum "$tmp/guest-O$opt" | cut -d' ' -f1)"
done
