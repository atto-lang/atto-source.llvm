#!/usr/bin/env bash
# Trusted-host-only source checkout gate; no cross-org git mirror, credentials,
# downloaded source artifacts, opaque "PASS" receipts, or guest parity claims.
# Usage: bash tests/tenantvm_actor_turn_exact_checkout.sh <40-hex-head> <g++|clang++> <optimized|asan-ubsan>
set -euo pipefail

if [[ "$#" -ne 3 || ! "$1" =~ ^[[:xdigit:]]{40}$ ]]; then
  echo "usage: $0 EXACT_HEAD_SHA g++|clang++ optimized|asan-ubsan" >&2
  exit 2
fi
expected="${1,,}"
cxx="$2"
mode="$3"
case "$cxx" in
  g++|clang++) ;;
  *) echo "unsupported compiler: $cxx" >&2; exit 2 ;;
esac
case "$mode" in
  optimized|asan-ubsan) ;;
  *) echo "unsupported test mode: $mode" >&2; exit 2 ;;
esac
command -v "$cxx" >/dev/null
command -v timeout >/dev/null

root="$(git rev-parse --show-toplevel)"
cd "$root"
actual="$(git rev-parse --verify HEAD)"
if [[ "$actual" != "$expected" ]]; then
  echo "checked-out commit mismatch (expected $expected, got $actual)" >&2
  exit 1
fi
# Exact-tree checks include all uncommitted tracked and untracked material.
# An untracked source shadow can change the compile or include search graph.
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
  echo "refusing dirty, untracked, or locally modified source tree" >&2
  exit 1
fi
for file in \
  src/main/cpp/oreslang/runtime/OresVM.hpp \
  src/main/cpp/oreslang/runtime/OresIsolateVM.hpp \
  src/main/cpp/oreslang/runtime/OresIsolateAbi.h \
  tests/native_tenantvm_actor_turn_object_test.cpp
do
  if [[ ! -f "$file" || -L "$file" ]]; then
    echo "expected regular non-symlink source file: $file" >&2
    exit 1
  fi
  git_blob="$(git rev-parse --verify "HEAD:$file")"
  disk_blob="$(git hash-object -- "$file")"
  if [[ "$git_blob" != "$disk_blob" ]]; then
    echo "source blob mismatch for $file" >&2
    exit 1
  fi
  printf 'EXACT_BLOB %s %s\n' "$file" "$disk_blob"
done

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
args=(-std=c++17 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
      -Isrc/main/cpp)
if [[ "$mode" == optimized ]]; then
  args+=(-O2)
else
  args+=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"$cxx" "${args[@]}" tests/native_tenantvm_actor_turn_object_test.cpp \
  -o "$tmp/tenantvm-actor-turn"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  timeout 60 "$tmp/tenantvm-actor-turn"
printf 'EXACT_HEAD_TENANTVM_ACTOR_TURN_HOST_PASS head=%s compiler=%s mode=%s\n' \
  "$expected" "$cxx" "$mode"
