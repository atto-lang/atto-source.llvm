#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
g++ -std=c++20 -Wall -Wextra -Werror -pedantic -fsanitize=undefined -fno-sanitize-recover=all \
  -I"$root/src/main/cpp/oreslang/backend" "$root/tests/named-recursive-native-ir/emit.cpp" -o emit
./emit invalid
for mode in fib count plus order; do
  ./emit "$mode" > "$mode.ll"
  for opt in 0 2; do
    clang -Wno-override-module -O"$opt" -c "$mode.ll" -o "$mode-$opt.o"
    clang -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all \
      "$root/tests/named-recursive-native-ir/host.c" "$mode-$opt.o" -o "$mode-$opt"
  done
done
for opt in 0 2; do
  for spec in 'fib 0 0 0' 'fib 1 0 1' 'fib 10 0 55' 'fib 20 0 6765' \
    'count 1024 0 1024' 'count 1025 6 0' \
    'plus 9223372036854775806 0 9223372036854775807' 'plus 9223372036854775807 1 0' 'order 0 1 0'; do
    read -r mode x status val <<< "$spec"
    timeout 5s "./$mode-$opt" "$x" "$status" "$val"
    echo "PASS $opt $spec"
  done
done
