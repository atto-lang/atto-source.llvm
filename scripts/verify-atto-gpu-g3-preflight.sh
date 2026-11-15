#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
head="$(git rev-parse HEAD)"
tree="$(git rev-parse HEAD^{tree})"
if [[ -n "${ATTO_EXPECTED_HEAD_SHA:-}" && "$head" != "$ATTO_EXPECTED_HEAD_SHA" ]]; then
  echo 'GPU_G3_PRECHECK_FAIL: expected exact head mismatch' >&2; exit 1
fi
for path in 'tests/atto_gpu_physical_saxpy_probe.cpp' 'scripts/verify-atto-gpu-g3-preflight.sh'; do
  [[ "$(git rev-parse "HEAD:$path")" == "$(git hash-object "$path")" ]] || {
    echo "GPU_G3_PRECHECK_FAIL: modified source $path" >&2; exit 1;
  }
done
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  tests/atto_gpu_physical_saxpy_probe.cpp -ldl -o "$work/gpu-physical-probe"
# A nonexistent kernel artifact must fail regardless of whether GPU hardware
# is installed. This is NEVER treated as a device-execution success.
if "$work/gpu-physical-probe" "$work/nonexistent.cl" >"$work/out" 2>"$work/error"; then
  echo 'GPU_G3_PRECHECK_FAIL: probe admitted nonexistent source' >&2; exit 1
fi
grep -q 'GPU_PHYSICAL_PROBE_FAILED' "$work/error"
if grep -q 'SAXPY_PASS' "$work/out" "$work/error"; then
  echo 'GPU_G3_PRECHECK_FAIL: false device success marker' >&2; exit 1
fi
printf 'source_head %s\nsource_tree %s\nevidence_class HOST_NEGATIVE_PROBE_ONLY\n' "$head" "$tree"
printf 'physical_gpu_executed false\nG3_HOST_PRECHECK_PASS\n'
