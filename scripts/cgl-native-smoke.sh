#!/usr/bin/env bash
# Prove only native scalar host-subset execution on a Linux-class host.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
build_dir="$root/build"
if [[ $# -gt 0 ]]; then build_dir="$1"; fi
compiler="$build_dir/oreslang-llvmc"
if [[ ! -x "$compiler" ]]; then
  echo "missing oreslang-llvmc in $build_dir" >&2
  exit 69
fi
command -v timeout >/dev/null || { echo "GNU timeout is required for bounded CGL execution" >&2; exit 69; }
python3 "$root/scripts/cgl-preflight.py"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
timeout --signal=TERM --kill-after=5s 180s env ORESLANG_LLVMC="$compiler" \
  "$root/scripts/compile-aot.sh" "$root/tests/fixtures/forward-call-main.ores" "$work/cgl-scalar"
python3 "$root/scripts/cgl-preflight.py" --binary "$work/cgl-scalar"
set +e
timeout --signal=TERM --kill-after=5s 30s "$work/cgl-scalar"
status=$?
set -e
if [[ "$status" -ne 42 ]]; then
  echo "expected native source program exit 42; got $status" >&2
  exit 1
fi
echo "CGL PILOT SMOKE PASS: host native scalar AOT (not actor RT or carrier-grade certification)"
