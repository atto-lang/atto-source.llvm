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
echo ATTO_MAIN_COMPILER_ATOMIC_I64_AOT_PASS
