#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT
for compiler in clang gcc; do
  command -v "$compiler" >/dev/null || { echo "missing compiler: $compiler" >&2; exit 1; }
  for opt in 0 2; do
    "$compiler" -std=c11 -O"$opt" -g -Wall -Wextra -Wpedantic -Werror \
      -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
      -I "$root/src/main/cpp" \
      "$root/tests/native_arc_control_v1_test.c" \
      "$root/src/main/cpp/oreslang/runtime/ArcControlV1.c" \
      -o "$dir/arc-${compiler}-O${opt}"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 "$dir/arc-${compiler}-O${opt}"
  done
done
