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
"$cc_bin" -std=c11 $common -Wall -Wextra -Wpedantic -Werror -DORES_NATIVE_THREAD_TEST_ID_EXHAUSTION -pthread \
  -I"$root/src/main/cpp" -c "$root/src/main/cpp/oreslang/runtime/NativeThreadV1.c" \
  -o "$tmp/NativeThreadV1.o"
"$cc_bin" -std=c11 $common -Wall -Wextra -Wpedantic -Werror -DORES_NATIVE_THREAD_TEST_ID_EXHAUSTION -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_thread_v1_test.c" "$tmp/NativeThreadV1.o" \
  -o "$tmp/native-thread-test"

"$cc_bin" -std=c11 $common -Wall -Wextra -Wpedantic -Werror \
  -DORES_NATIVE_THREAD_TEST_ID_EXHAUSTION -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_thread_scope_v1_test.c" \
  "$tmp/NativeThreadV1.o" -o "$tmp/native-thread-scope-test"
"$tmp/native-thread-scope-test"
"$tmp/native-thread-test"
"$cxx_bin" -std=c++17 $common -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_thread_capture_admission_test.cpp" \
  -o "$tmp/capture-admission-test"
"$tmp/capture-admission-test"
"$cxx_bin" -std=c++17 $common -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_thread_join_reaper_v1_test.cpp" \
  "$tmp/NativeThreadV1.o" -o "$tmp/thread-join-reaper-test"
"$tmp/thread-join-reaper-test"

# Host-only scheduler notification bridge: cancellation, exactly-once enqueue,
# generation tags, and race with physical thread completion.
"$cxx_bin" -std=c++17 $common -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_join_task_wait_v1_test.cpp" \
  "$tmp/NativeThreadV1.o" -o "$tmp/native-join-task-wait"
"$tmp/native-join-task-wait"


# End-to-end host-only join completion -> Task READY -> exact scheduler claim.
# Keep all C stores compiled as C, and never run guest frames from the reaper.
for unit in NativeExecutionContextStore NativeContinuationStore NativeAwaitTraceBridge; do
  "$cc_bin" -std=c11 $common -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$root/src/main/cpp" -c "$root/src/main/cpp/oreslang/runtime/$unit.c" \
    -o "$tmp/$unit.o"
done
"$cxx_bin" -std=c++17 $common -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_join_task_continuation_bridge_v1_test.cpp" \
  "$tmp/NativeThreadV1.o" "$tmp/NativeExecutionContextStore.o" \
  "$tmp/NativeContinuationStore.o" "$tmp/NativeAwaitTraceBridge.o" \
  -o "$tmp/native-join-task-continuation-bridge"
"$tmp/native-join-task-continuation-bridge"

echo "native pthread sanitizer suite: host-only PASS"
