#!/usr/bin/env sh
# Host-only pthread ABI test. Does not require LLVM libraries or execute guest code.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc_bin="${CC:-cc}"
"$cc_bin" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
    -I"$root/src/main/cpp" \
    "$root/src/main/cpp/oreslang/runtime/NativeThreadV1.c" \
    "$root/tests/native_thread_v1_test.c" \
    -o "$tmp/native-thread-v1"
"$tmp/native-thread-v1"

# A header-only compiler-admission model test. Does not invoke guest frontend.
case "${CC:-cc}" in
  gcc) cxx_bin=g++ ;;
  clang) cxx_bin=clang++ ;;
  *) cxx_bin="${CXX:-c++}" ;;
esac
"$cxx_bin" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I"$root/src/main/cpp" \
    "$root/tests/native_thread_capture_admission_test.cpp" \
    -o "$tmp/native-thread-capture-admission"
"$tmp/native-thread-capture-admission"
