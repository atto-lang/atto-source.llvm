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
python3 "$root/scripts/cgl-preflight.py"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
ORESLANG_LLVMC="$compiler" "$root/scripts/compile-aot.sh" "$root/tests/fixtures/forward-call-main.ores" "$work/cgl-scalar"
python3 "$root/scripts/cgl-preflight.py" --binary "$work/cgl-scalar"
set +e
"$work/cgl-scalar"
status=$?
set -e
if [[ "$status" -ne 42 ]]; then
  echo "expected native source program exit 42; got $status" >&2
  exit 1
fi
echo "CGL PILOT SMOKE PASS: host native scalar AOT (not actor RT or carrier-grade certification)"
