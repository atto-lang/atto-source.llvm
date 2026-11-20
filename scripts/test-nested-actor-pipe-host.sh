#!/usr/bin/env bash
# LLVM-source-independent C++17 metadata admission. NOT .atto execution.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for cmd in clang++ g++; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    printf 'REQUIRED_COMPILER_MISSING: %s\n' "$cmd" >&2
    exit 2
  fi
done
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
source="$root/tests/native_nested_actor_pipeline_plan_test.cpp"
include="$root/src/main/cpp"
flags=(-std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -I "$include")
for cmd in clang++ g++; do
  for opt in 0 2; do
    exe="$out/${cmd//+/p}-O${opt}"
    "$cmd" "${flags[@]}" "-O${opt}" "$source" -o "$exe"
    "$exe"
    printf 'PASS source-free host planner: %s O%s\n' "$cmd" "$opt"
  done
done
exe="$out/clang-sanitized"
clang++ "${flags[@]}" -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer "$source" -o "$exe"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$exe"
printf 'PASS source-free host planner: clang++ ASan+UBSan\n'
printf 'SOURCE_FREE_ONLY: no .atto parser, compiler, actor scheduler or guest runtime executed\n'
