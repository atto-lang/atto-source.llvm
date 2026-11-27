#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
compiler="$1"
clang="$2"
source="$(cd "$(dirname "$0")" && pwd)/fixtures/unicode-identifiers-v1/guest.atto"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
timeout 60 "$compiler" --target=host "$source" > "$tmp/guest.ll"
test -s "$tmp/guest.ll"
for opt in 0 2; do
  timeout 60 "$clang" "-O$opt" -x ir "$tmp/guest.ll" -o "$tmp/guest"
  status=0
  timeout 20 "$tmp/guest" || status=$?
  test "$status" -eq 100
  echo "UNICODE_NATIVE_GUEST_O${opt}_RESULT=$status"
done
for bad in 'pub fnc main(): int { return ¬1 ? 42 : 0; }' 'pub fnc main(): int { return 未定義; }' 'pub fnc main(): int { return true¬ ? 42 : 0; }' 'pub fnc main(): int { return true ¬eq false ? 42 : 0; }'; do
  printf '%s\n' "$bad" > "$tmp/bad.atto"
  status=0
  timeout 30 "$compiler" --target=host "$tmp/bad.atto" > "$tmp/bad.ll" 2>"$tmp/bad.err" || status=$?
  if test "$status" -ne 1; then
    echo "expected normal compiler diagnostic exit 1, observed $status" >&2
    cat "$tmp/bad.err" >&2
    exit 1
  fi
  test ! -s "$tmp/bad.ll"
  test -s "$tmp/bad.err"
done
echo UNICODE_NATIVE_NEGATIVES_PASS
