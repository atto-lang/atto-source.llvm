#!/usr/bin/env bash
set -euo pipefail
compiler="${CXX:-c++}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
for scenario in PUBLIC_ISSUE FLOAT BOOL; do
  if "$compiler" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
      -I"$repo_root/src/main/cpp" -D"ORESLANG_TEST_NEGATIVE_$scenario" \
      -fsyntax-only "$repo_root/tests/native_region_negative_compile.cpp" >"$log" 2>&1; then
    echo "FAIL: $scenario was unexpectedly admitted" >&2
    exit 1
  fi
  if [[ "$scenario" == PUBLIC_ISSUE ]]; then
    pattern='private'
  else
    pattern='unsigned 8/16/32/64-bit scalar'
  fi
  if ! grep -Fq "$pattern" "$log"; then
    echo "FAIL: $scenario rejected for unexpected reason" >&2
    cat "$log" >&2
    exit 1
  fi
  echo "PASS: $scenario rejected at compile-time"
done
