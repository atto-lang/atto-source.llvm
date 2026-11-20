#!/usr/bin/env bash
# Actual source .ores -> oreslang-llvmc -> LLVM IR -> Clang native executable
# -> native scheduler/owned scalar state; no C++ or Java mock KV.
# Python is not used. Full native Bytes+network Atto remains fail closed.
set -euo pipefail
if [[ $# != 3 ]]; then
  echo "usage: $0 <production-llvmc> <clang> <repo-root>" >&2
  exit 2
fi
compiler="$1"; clang="$2"; root="$3"
[[ -x "$compiler" ]]
command -v "$clang" >/dev/null
command -v rustc >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
rustc --edition=2021 -D warnings \
  "$root/tests/atto_redis_native_scalar_guest.rs" -o "$tmp/atto-redis-llvm-guest-check"
timeout 90s "$tmp/atto-redis-llvm-guest-check" \
  "$compiler" "$clang" "$root" "$tmp"
