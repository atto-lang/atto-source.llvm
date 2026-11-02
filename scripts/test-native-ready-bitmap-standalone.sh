#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
for cc in gcc clang; do
  if ! command -v "$cc" >/dev/null; then echo "Missing $cc" >&2; exit 2; fi
  "$cc" -std=c11 -O1 -g -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo_root/src/main/cpp" \
    "$repo_root/tests/native_ready_bitmap_test.c" -o "$work_dir/native_bitmap_$cc"
  "$work_dir/native_bitmap_$cc"
done
