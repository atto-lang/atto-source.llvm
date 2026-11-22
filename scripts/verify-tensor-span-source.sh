#!/usr/bin/env bash
# Actual Oreslang .ores -> native LLVM compiler -> Clang -> OS execution.
# Reuses production scripts/compile-aot.sh; no handwritten LLVM IR or host stub.
set -euo pipefail
[[ $# == 2 ]] || { echo 'usage: verify-tensor-span-source.sh LLVM_COMPILER CLANG' >&2; exit 64; }
compiler="$1"
clang="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -x "$compiler" ]] || { echo "Oreslang compiler is not executable: $compiler" >&2; exit 69; }
command -v "$clang" >/dev/null || { echo "Clang missing: $clang" >&2; exit 69; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
fixture="$root/tests/fixtures/native-tensor-span-boundary.ores"
[[ -s "$fixture" ]] || { echo 'Missing actual .ores fixture' >&2; exit 66; }

ORESLANG_LLVMC="$compiler" CLANG="$clang" \
  bash "$root/scripts/compile-aot.sh" "$fixture" "$work/tensor-span"
[[ -x "$work/tensor-span" ]] || { echo 'No native executable emitted' >&2; exit 1; }
if "$work/tensor-span" >"$work/stdout" 2>"$work/stderr"; then
  code=0
else
  code=$?
fi
if [[ $code -ne 42 ]]; then
  echo "Compiled tensor span guest returned $code, expected 42" >&2
  cat "$work/stdout" "$work/stderr" >&2
  exit 1
fi
if [[ -s "$work/stdout" || -s "$work/stderr" ]]; then
  echo 'Compiled tensor span guest unexpectedly produced output' >&2
  cat "$work/stdout" "$work/stderr" >&2
  exit 1
fi
sha256sum "$fixture" "$compiler" "$work/tensor-span"
echo 'TENSOR-SPAN-GUEST-LLVM-OK'
