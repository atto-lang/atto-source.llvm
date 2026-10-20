#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-c++}"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
"$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -O2 \
  -I "$root/src/main/cpp" \
  "$root/tests/fsm_layout_alternatives_test.cpp" -o "$build/fsm-layout-proof"
"$build/fsm-layout-proof" | grep -Fx \
  'FSM_LAYOUT_HOST_PASS traces=1023 layouts=3 rows=5 depth=4 negative=guard,overflow,loans,stale-view,owner,generation,ecs'
