#!/usr/bin/env bash
# Exact checked-in native owned-port proofs, independent of LLVM toolchain.
# CXX=clang++ SANITIZE=address bash scripts/test-bounded-actor-port.sh
# CXX=g++ SANITIZE=thread bash scripts/test-bounded-actor-port.sh
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${CXX:-g++}"
mode="${SANITIZE:-none}"
command -v "$compiler" >/dev/null 2>&1 || { echo "missing $compiler" >&2; exit 2; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
flags=(-std=c++20 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror
       -I"$root/src/main/cpp")
case "$mode" in
  none) ;;
  address) flags+=(-fno-omit-frame-pointer -fsanitize=address,undefined)
           export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}"
           export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1}" ;;
  thread) flags+=(-fno-omit-frame-pointer -fsanitize=thread)
          export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1}" ;;
  *) echo "SANITIZE must be none, address or thread" >&2; exit 2 ;;
esac
for suite in bounded_actor_port_test bounded_actor_port_concurrency_test; do
  "$compiler" "${flags[@]}" "$root/tests/$suite.cpp" -o "$work/$suite"
  timeout 60 "$work/$suite"
done
printf 'BOUNDED_ACTOR_PORT_TEST_PASS compiler=%s sanitize=%s\n' "$compiler" "$mode"
