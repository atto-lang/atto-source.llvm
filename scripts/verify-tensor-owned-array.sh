#!/usr/bin/env bash
# Execute compiler-generated Oreslang source using the existing checked
# Array<int> native region runtime. This is NOT a host-only tensor oracle.
set -euo pipefail
[[ $# == 2 ]] || { echo "usage: verify-tensor-owned-array.sh LLVM_COMPILER CLANG" >&2; exit 64; }
compiler="$1"
clang="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
fixture="$root/tests/fixtures/native-tensor-owned-array.ores"
test -f "$fixture" || { echo "missing Oreslang tensor guest source" >&2; exit 66; }
test -x "$compiler" || { echo "real LLVM compiler missing" >&2; exit 69; }
command -v "$clang" >/dev/null || { echo "Clang missing" >&2; exit 69; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
if ! ORESLANG_LLVMC="$compiler" CLANG="$clang" \
    bash "$root/scripts/compile-aot.sh" "$fixture" "$work/tensor-guest"; then
  echo "failed actual Oreslang->LLVM->Clang compilation" >&2
  exit 1
fi
test -x "$work/tensor-guest"
set +e
"$work/tensor-guest" >"$work/stdout" 2>"$work/stderr"
status=$?
set -e
if [[ $status -ne 42 || -s "$work/stdout" || -s "$work/stderr" ]]; then
  echo "tensor memory guest result=$status expected 42 with silent output" >&2
  cat "$work/stdout" "$work/stderr" >&2
  exit 1
fi
sha256sum "$fixture" "$compiler" "$work/tensor-guest"
printf '%s\n' 'TENSOR-OWNED-ARRAY-GUEST-LLVM-OK'
