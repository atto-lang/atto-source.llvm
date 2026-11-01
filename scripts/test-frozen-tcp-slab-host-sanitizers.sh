#!/usr/bin/env bash
set -euo pipefail
# Executes real native C++ host slab code, not compiled guest actor code.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/src/main/cpp" "$ROOT/tests/frozen_tcp_slab_test.cpp" \
  -o "$WORK/frozen_tcp_slab_test"
ASAN_OPTIONS=detect_leaks=1 "$WORK/frozen_tcp_slab_test"
printf '%s\n' 'NATIVE_FROZEN_TCP_SLAB_HOST_SANITIZERS_PASS'
