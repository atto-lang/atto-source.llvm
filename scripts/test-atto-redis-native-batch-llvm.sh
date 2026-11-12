#!/usr/bin/env bash
# Real production oreslang-llvmc -> LLVM IR -> Clang native actor scheduler.
# Four Redis-style scalar commands per native mailbox message; no host KV.
# This is NOT the final binary Bytes / RESP2 / native TCP compiler feature.
set -euo pipefail
if [[ $# != 3 ]]; then
  echo "usage: $0 <compiler> <clang> <repo-root>" >&2
  exit 2
fi
compiler="$1"; clang="$2"; root="$3"
[[ -x "$compiler" ]]
command -v "$clang" >/dev/null
command -v rustc >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
rustc --edition=2021 -D warnings \
  "$root/tests/atto_redis_native_batch_guest.rs" -o "$tmp/atto-redis-llvm-batch-guest"
timeout 110s "$tmp/atto-redis-llvm-batch-guest" \
  "$compiler" "$clang" "$root" "$tmp"
