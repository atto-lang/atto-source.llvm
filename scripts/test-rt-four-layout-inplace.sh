#!/usr/bin/env sh
# Real native host binary (not Oreslang guest nor hardware WCET).
# No LLVM toolchain needed: compile C arena as C11, C++ kernel as C++17.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cc=${CC:-cc}
cxx=${CXX:-c++}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
for opt in 0 2; do
  "$cc" -std=c11 -O"$opt" -Wall -Wextra -Wpedantic -Werror \
    -I "$root/src/main/cpp" \
    -c "$root/src/main/cpp/oreslang/runtime/RealtimeActorArena.c" \
    -o "$tmp/arena.o"
  "$cxx" -std=c++17 -O"$opt" -Wall -Wextra -Wpedantic -Werror \
    -UNDEBUG -pthread -I "$root/src/main/cpp" \
    "$root/tests/realtime_four_layout_inplace_kernel_test.cpp" \
    "$tmp/arena.o" -o "$tmp/four-layout-kernel"
  "$tmp/four-layout-kernel"
done
printf '%s\n' 'PASS: native host bounded four-layout in-place kernel (NOT hard RT certified)'
