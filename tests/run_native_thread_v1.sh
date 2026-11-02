#!/usr/bin/env sh
# Host-only pthread ABI test. Does not require LLVM libraries or execute guest code.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc_bin="${CC:-cc}"
"$cc_bin" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -DORES_NATIVE_THREAD_TEST_ID_EXHAUSTION -pthread \
    -I"$root/src/main/cpp" \
    "$root/src/main/cpp/oreslang/runtime/NativeThreadV1.c" \
    "$root/tests/native_thread_v1_test.c" \
    -o "$tmp/native-thread-v1"

# Scope quota test reuses the exact same native C runtime and test-only
# exhaustion hook; no guest compiler or external source copy is involved.
"$cc_bin" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -DORES_NATIVE_THREAD_TEST_ID_EXHAUSTION -pthread \
    -I"$root/src/main/cpp" \
    "$root/src/main/cpp/oreslang/runtime/NativeThreadV1.c" \
    "$root/tests/native_thread_scope_v1_test.c" \
    -o "$tmp/native-thread-scope-v1"
"$tmp/native-thread-scope-v1"
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

# The C host runtime is built with the C compiler, then linked into the
# bounded C++17 reaper model. This tests true native thread join cleanup.
"$cc_bin" -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
    -I"$root/src/main/cpp" \
    -c "$root/src/main/cpp/oreslang/runtime/NativeThreadV1.c" \
    -o "$tmp/NativeThreadV1.o"
"$cxx_bin" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
    -I"$root/src/main/cpp" \
    "$root/tests/native_thread_join_reaper_v1_test.cpp" \
    "$tmp/NativeThreadV1.o" \
    -o "$tmp/native-thread-join-reaper"
"$tmp/native-thread-join-reaper"

# Host-only scheduler notification bridge: cancellation, exactly-once enqueue,
# generation tags, and race with physical thread completion.
"$cxx_bin" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_join_task_wait_v1_test.cpp" \
  "$tmp/NativeThreadV1.o" -o "$tmp/native-join-task-wait"
"$tmp/native-join-task-wait"


# End-to-end host-only join completion -> Task READY -> exact scheduler claim.
# Keep all C stores compiled as C, and never run guest frames from the reaper.
for unit in NativeExecutionContextStore NativeContinuationStore NativeAwaitTraceBridge; do
  "$cc_bin" -std=c11 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$root/src/main/cpp" -c "$root/src/main/cpp/oreslang/runtime/$unit.c" \
    -o "$tmp/$unit.o"
done
"$cxx_bin" -std=c++17 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
  -I"$root/src/main/cpp" "$root/tests/native_join_task_continuation_bridge_v1_test.cpp" \
  "$tmp/NativeThreadV1.o" "$tmp/NativeExecutionContextStore.o" \
  "$tmp/NativeContinuationStore.o" "$tmp/NativeAwaitTraceBridge.o" \
  -o "$tmp/native-join-task-continuation-bridge"
"$tmp/native-join-task-continuation-bridge"
