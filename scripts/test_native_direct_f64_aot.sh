#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
out="$(mktemp -d)"
trap 'rm -rf -- "$out"' EXIT
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
export ORESLANG_LLVMC="$compiler" ORES_NO_GC_IR_AUDIT="$auditor" CLANG="$clang"
echo "DIRECT_F64_COMPILER_SHA256=$(sha256sum "$compiler" | cut -d' ' -f1)"
for opt in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  if ! bash "$driver" "$root/tests/native-direct-array-f64/positive.atto" "$out/guest-O$opt" >"$out/build.out" 2>"$out/build.err"; then
    cat "$out/build.out" "$out/build.err" >&2
    echo "DIRECT_F64_COMPILATION_FAILED=O$opt" >&2
    exit 1
  fi
  test "$(head -c 4 "$out/guest-O$opt" | od -An -tx1 | tr -d ' \n')" = "7f454c46"
  set +e
  timeout 15s "$out/guest-O$opt" >"$out/guest.out" 2>"$out/guest.err"
  status=$?
  set -e
  test "$status" -eq 42 || { cat "$out/guest.err"; echo "GUEST_EXIT=$status" >&2; exit 1; }
  test ! -s "$out/guest.out"
  test ! -s "$out/guest.err"
  echo "DIRECT_BORROWED_F64_AOT_GUEST_PASS=O$opt;sha256=$(sha256sum "$out/guest-O$opt" | cut -d' ' -f1)"
done
for f in alias shared-mutation resize value element; do
  if "$ORESLANG_LLVMC" --target=host "$root/tests/native-direct-array-f64/$f.atto" > "$out/reject.ll" 2>"$out/reject.err"; then
    echo "UNSAFE_DIRECT_ARRAY_F64_ADMITTED=$f" >&2
    exit 1
  fi
  test ! -s "$out/reject.ll"
  test -s "$out/reject.err"
  case "$f" in
    alias) expected="conflicting active native borrow";;
    shared-mutation) expected="ownership violation: Array<int> cannot be accessed through a borrowed class";;
    resize) expected="ownership violation: cannot resize borrowed Array<f64> handle";;
    value) expected="native Array<f64> parameters must explicitly borrow";;
    element) expected="native direct array borrow supports only Array<float>/Array<f64>";;
  esac
  grep -Fq "oreslang-llvmc: $expected" "$out/reject.err" || {
    echo "NOT_THE_EXPECTED_OWNERSHIP_REJECTION=$f" >&2
    cat "$out/reject.err" >&2
    exit 1
  }
  echo "DIRECT_BORROWED_F64_NEGATIVE_NO_IR=$f;$(head -n 1 "$out/reject.err")"
done
if "$ORESLANG_LLVMC" --target=wasm32 "$root/tests/native-direct-array-f64/positive.atto" > "$out/wasm.ll" 2>"$out/wasm.err"; then
  echo "UNVERIFIED_WASM_ARRAY_REGION_ABI_ADMITTED" >&2
  exit 1
fi
test ! -s "$out/wasm.ll"
grep -Fq "native direct Array<f64> borrowing requires host region ABI" "$out/wasm.err"
echo "DIRECT_ARRAY_BORROW_WASM32_FAIL_CLOSED_NO_IR"

