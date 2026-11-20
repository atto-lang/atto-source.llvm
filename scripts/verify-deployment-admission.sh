#!/usr/bin/env bash
# Standalone fail-closed profile regression. Does not require LLVM.
set -euo pipefail
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
compiler="${1:-c++}"
command -v "$compiler" >/dev/null || { echo "missing compiler: $compiler" >&2; exit 2; }
build_dir="$(mktemp -d)"
trap 'rm -rf -- "$build_dir"' EXIT
"$compiler" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I src/main/cpp tests/deployment_profile_admission_test.cpp \
  -o "$build_dir/deployment-profile-admission"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
  "$build_dir/deployment-profile-admission"
echo "PASS: standalone deployment profile safety assertions ($compiler)"
