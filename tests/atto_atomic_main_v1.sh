#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="./build/oreslang-llvmc"
if [ "$#" -gt 0 ]; then compiler="$1"; fi
command -v clang >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for name in main-int main-int-default; do
  "$compiler" "tests/atto-atomic-main-v1/$name.atto" >"$tmp/$name.ll"
  grep -q 'load atomic i64' "$tmp/$name.ll"
  grep -q 'store atomic i64' "$tmp/$name.ll"
  clang -x ir -Wno-override-module "$tmp/$name.ll" -o "$tmp/$name"
  if "$tmp/$name"; then status=0; else status=$?; fi
  test "$status" -eq 42 || { echo "main AOT $name returned $status"; exit 1; }
done
grep -q 'atomicrmw add' "$tmp/main-int.ll"
for f in tests/atto-atomic-main-v1/reject-*.atto; do
  if "$compiler" "$f" >"$tmp/reject.ll" 2>"$tmp/reject.err"; then
    echo "unsafe native Atomic fixture compiled: $f" >&2; exit 1
  fi
  test ! -s "$tmp/reject.ll"
done
# The first hosted i64 atomic path must never silently lower to a
# non-threaded Wasm, Cortex-M, or unsupported pointer ABI.
for target in wasm32 thumbv7m-none-eabi; do
  if "$compiler" "--target=$target" tests/atto-atomic-main-v1/main-int.atto >"$tmp/unsupported.ll" 2>"$tmp/unsupported.err"; then
    echo "unsupported atomic AOT target admitted: $target" >&2
    exit 1
  fi
  test ! -s "$tmp/unsupported.ll"
done
echo ATTO_MAIN_COMPILER_ATOMIC_I64_AOT_PASS
