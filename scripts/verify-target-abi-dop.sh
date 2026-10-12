#!/usr/bin/env bash
# Standalone native safety gate: no LLVM development installation required.
# Run from anywhere: bash scripts/verify-target-abi-dop.sh [g++|clang++]
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
compiler="${1:-c++}"
command -v "$compiler" >/dev/null || { echo "missing C++ compiler: $compiler" >&2; exit 2; }

build_dir="$(mktemp -d)"
trap 'rm -rf -- "$build_dir"' EXIT

echo "Target: $(uname -m); C++ compiler: $("$compiler" --version | head -n 1)"
common=(-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror
        -fsanitize=address,undefined -fno-omit-frame-pointer
        -pthread -I src/main/cpp)

for fixture in target_abi_test dop_layout_test dop_kernel_lowering_test dop_hostbuffer_concurrency_test; do
  "$compiler" "${common[@]}" "tests/${fixture}.cpp" -o "$build_dir/$fixture"
  if [[ "$fixture" == target_abi_test ]]; then
    "$build_dir/$fixture" contracts/architecture/v1/targets.tsv > "$build_dir/$fixture.log"
  else
    "$build_dir/$fixture" > "$build_dir/$fixture.log"
  fi
  printf 'PASS %s (%s)\n' "$fixture" "$compiler"
done

echo "PASS: exact checked-target, native DOP layout, and F32 ownership regression suites"
