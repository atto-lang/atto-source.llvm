#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
command -v clang >/dev/null
command -v g++ >/dev/null
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT
g++ -std=c++20 -O2 -Wall -Wextra -Werror -Isrc/main/cpp \
  src/main/cpp/oreslang/frontend/NativeAtomicAotV1.cpp -o "$dir/atto-atomic-aot"
for name in positive-int positive-cas positive-bool; do
  "$dir/atto-atomic-aot" "tests/atto-atomic-aot-v1/$name.atto" "$dir/$name.ll"
  grep -qE ' (load|store) atomic |atomicrmw|cmpxchg' "$dir/$name.ll"
  clang -x ir -Wno-override-module "$dir/$name.ll" -o "$dir/$name"
  if "$dir/$name"; then rc=0; else rc=$?; fi
  if [ "$rc" -ne 42 ]; then
    echo "Wrong native AOT result: $name returned $rc, expected 42" >&2
    exit 1
  fi
done
grep -q 'atomicrmw add' "$dir/positive-int.ll"
grep -q 'cmpxchg' "$dir/positive-cas.ll"
grep -q 'i8' "$dir/positive-bool.ll"
for file in tests/atto-atomic-aot-v1/reject-*.atto; do
  rm -f "$dir/reject.ll"
  if "$dir/atto-atomic-aot" "$file" "$dir/reject.ll" >"$dir/reject.log" 2>&1; then
    echo "Unsafe source admitted: $file" >&2; exit 1
  fi
  test ! -e "$dir/reject.ll"
  grep -q 'ATTO_ATOMIC_AOT_V1_REJECT' "$dir/reject.log"
done
echo 'ATTO_ATOMIC_GUEST_NATIVE_AOT_V1_PASS'
