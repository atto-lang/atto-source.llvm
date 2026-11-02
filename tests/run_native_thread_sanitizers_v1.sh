#!/usr/bin/env sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc_bin="${CC:-cc}"
case "$cc_bin" in
  gcc) cxx_bin=g++ ;;
  clang) cxx_bin=clang++ ;;
  *) cxx_bin="${CXX:-c++}" ;;
esac
common="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -UNDEBUG"
export ASAN_OPTIONS="detect_leaks=1:halt_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
"$cc_bin" -std=c11 $common -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" -c "$root/src/main/cpp/oreslang/runtime/NativeThreadV1.c" \
  -o "$tmp/NativeThreadV1.o"
"$cc_bin" -std=c11 $common -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_thread_v1_test.c" "$tmp/NativeThreadV1.o" \
  -o "$tmp/native-thread-test"
"$tmp/native-thread-test"
"$cxx_bin" -std=c++17 $common -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_thread_capture_admission_test.cpp" \
  -o "$tmp/capture-admission-test"
"$tmp/capture-admission-test"
"$cxx_bin" -std=c++17 $common -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_thread_join_reaper_v1_test.cpp" \
  "$tmp/NativeThreadV1.o" -o "$tmp/thread-join-reaper-test"
"$tmp/thread-join-reaper-test"
echo "native pthread sanitizer suite: host-only PASS"
