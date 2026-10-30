#!/usr/bin/env sh
# Standalone compiler-backed RT structural gate, no LLVM installation needed.
# Compiler output is HOST contract evidence, not guest, WCET or no-GC proof.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -z "${CXX:-}" ]; then CXX=c++; fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
for opt in 0 2; do
    "$CXX" -std=c++17 -O"$opt" -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
        -I "$root/src/main/cpp" "$root/tests/rt_admission_bounds_test.cpp" \
        -o "$tmp/rt-admission-$opt"
    "$tmp/rt-admission-$opt"
done
printf '%s\n' 'PASS: host-only LLVM RT declared-bound contract (NOT WCET certified)'
