#!/usr/bin/env bash
# Trusted source-repository exact Git-blob integration preflight. This does
# not authorize or perform cross-repository export or certify guest execution.
# Source #1896's compiler/ABI files and #1897's actor-turn runtime files were
# combined from exact blob IDs into this integrated review branch.
set -euo pipefail

expected_head="${1:-}"
if [[ ! "$expected_head" =~ ^[[:xdigit:]]{40}$ ]]; then
  echo "TENANTVM_UNIFIED_SOURCE_SHA_REQUIRED" >&2
  exit 2
fi
root="$(git rev-parse --show-toplevel)"
cd "$root"
if [[ "$(git rev-parse HEAD)" != "${expected_head,,}" ]]; then
  echo "TENANTVM_UNIFIED_WRONG_HEAD" >&2
  exit 1
fi
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
  echo "TENANTVM_UNIFIED_DIRTY_TREE" >&2
  exit 1
fi

# Entries MUST be updated through independent review if a source capability
# contract changes. A missing/renamed/mode-changed file fails closed.
while IFS=' ' read -r blob path; do
  [[ -n "$blob" ]] || continue
  if [[ ! -f "$path" || -L "$path" ]]; then
    echo "TENANTVM_UNIFIED_MISSING_FILE $path" >&2
    exit 1
  fi
  actual="$(git rev-parse --verify "HEAD:$path")"
  disk="$(git hash-object -- "$path")"
  mode="$(git ls-tree HEAD -- "$path" | awk '{print $1}')"
  if [[ "$mode" != 100644 || "$actual" != "$blob" || "$disk" != "$blob" ]]; then
    echo "TENANTVM_UNIFIED_BLOB_OR_MODE_MISMATCH $path" >&2
    exit 1
  fi
  printf 'TENANTVM_EXACT_SOURCE_BLOB %s %s\n' "$path" "$blob"
done <<'PINNED_GIT_BLOBS'
7015598be04972d5e66f43e4554c4af896b3dc6e src/main/cpp/oreslang/backend/LlvmCodegen.cpp
e2e35dc461bdc5ae4f9bc5742181cdbccffcfc1a src/main/cpp/oreslang/main.cpp
ac05a911c1fcbe55a770171950931776b63a814f tests/fixtures/tenantvm-trusted-scalar.atto
e3bdfa9c901210692451220606184b0541c25310 tests/native_tenantvm_atto_guest_test.cpp
805d6951ee678cdab028f9685d4430fae3dfc8fb src/main/cpp/oreslang/runtime/OresVM.hpp
4cf332d75b3351b9cd4a03e9bb4922251e986542 src/main/cpp/oreslang/runtime/OresIsolateVM.hpp
0ab3d69d2be78d1b156e294b49ed22192c4de713 src/main/cpp/oreslang/runtime/OresIsolateAbi.h
01907996041ec4cd16a76f4ef7ca9be2a1065dfa tests/native_tenantvm_actor_turn_object_test.cpp
54b0186280db15b576943b82bee6d3369f71b168 tests/tenantvm_actor_turn_exact_checkout.sh
PINNED_GIT_BLOBS

printf 'TENANTVM_UNIFIED_SOURCE_IDENTITY_VERIFIED head=%s mode=HOST_AOT_REVIEW_ONLY\n' "$expected_head"
