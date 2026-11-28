#!/bin/sh
# Host-only evidence; no LLVM SDK, guest compiler, network or credentials.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
for cc in g++ clang++; do
  command -v "$cc" >/dev/null
  for opt in 0 2; do
    "$cc" -std=c++17 -O"$opt" -Wall -Wextra -Werror -pedantic \
      -fsanitize=address,undefined -fno-sanitize-recover=all -no-pie \
      -I"$root/src/main/cpp" "$root/tests/reversible_scalar_turn_v1_test.cpp" -o "$work/test"
    "$work/test"
  done
done
