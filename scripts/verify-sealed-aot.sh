#!/usr/bin/env bash
# Fail closed on a missing CTest registration; do not accept "No tests were found".
set -euo pipefail
[[ $# == 1 ]] || { echo 'usage: scripts/verify-sealed-aot.sh <cmake-build-dir>' >&2; exit 64; }
build="$1"
[[ -f "$build/CTestTestfile.cmake" ]] || {
  echo "No configured CTestTestfile.cmake in $build." >&2
  exit 66
}
for suite in \
  native_sealed_scalar_aot_v1 \
  native_sealed_class_fields_reachability_v1 \
  native_sealed_aot_profile_mutations
do
  ctest --test-dir "$build" --no-tests=error --output-on-failure -R "^$suite$"
done
echo 'All three exact sealed AOT CTests were registered and passed.'
