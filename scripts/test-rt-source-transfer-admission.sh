#!/usr/bin/env sh
# Reproducible compiler AST/RT admission test without LLVM packages.
# This checks real RealtimeAnalysis.hpp AST logic, not a compiled .ores guest.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cxx=${CXX:-c++}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
for opt in 0 2; do
    "$cxx" -std=c++17 -O"$opt" -UNDEBUG \
        -Wall -Wextra -Wpedantic -Werror \
        -I "$root/src/main/cpp" \
        "$root/tests/realtime_analysis_test.cpp" \
        -o "$tmp/realtime-analysis-$opt"
    "$tmp/realtime-analysis-$opt"
done
printf '%s\n' 'PASS: host compiler AST strict RT transfer gate (NOT guest/WCET certified)'
