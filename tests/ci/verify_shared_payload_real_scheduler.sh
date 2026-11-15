#!/usr/bin/env bash
# Exact-head native host integration verifier; no cross-organization copy.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

: "${EXPECTED_HEAD:?EXPECTED_HEAD is required}"
: "${CC:?CC is required}"
: "${CXX:?CXX is required}"
: "${OPT_LEVEL:?OPT_LEVEL is required}"
case "$OPT_LEVEL" in 1|2) ;; *) echo "invalid optimization" >&2; exit 2;; esac
case "$CC:$CXX" in gcc:g++|clang:clang++) ;; *) echo "unapproved toolchain" >&2; exit 2;; esac

head="$(git rev-parse HEAD)"
tree="$(git rev-parse 'HEAD^{tree}')"
if [[ "$head" != "$EXPECTED_HEAD" ]]; then
  echo "HEAD_MISMATCH actual=$head expected=$EXPECTED_HEAD" >&2
  exit 3
fi
test -z "$(git status --porcelain --untracked-files=no)" || {
  echo "DIRTY_CHECKOUT" >&2; exit 4;
}
echo "EXACT_SOURCE_HEAD=$head"
echo "EXACT_SOURCE_TREE=$tree"

files=(
  src/main/cpp/oreslang/runtime/NativeActorScheduler.h
  src/main/cpp/oreslang/runtime/NativeActorScheduler.c
  src/main/cpp/oreslang/runtime/NativeActorSharedPayloadBridge.hpp
  src/main/cpp/oreslang/runtime/SharedImmutablePayload.hpp
  tests/native_actor_shared_payload_scheduler_bridge_test.cpp
)
for file in "${files[@]}"; do
  expected_blob="$(git rev-parse "HEAD:$file")"
  actual_blob="$(git hash-object "$file")"
  [[ "$expected_blob" == "$actual_blob" ]] || {
    echo "BLOB_MISMATCH path=$file" >&2; exit 5;
  }
  echo "VERIFIED_GIT_BLOB $actual_blob $file"
done

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
flags=(-O"$OPT_LEVEL" -g -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer
       -fsanitize=address,undefined -fno-sanitize-recover=all -pthread)
"$CC" --version | head -n 1
"$CXX" --version | head -n 1

"$CC" -std=c11 "${flags[@]}" \
  -Isrc/main/cpp \
  -c src/main/cpp/oreslang/runtime/NativeActorScheduler.c \
  -o "$workdir/native-scheduler.o"
"$CXX" -std=c++20 "${flags[@]}" \
  -Isrc/main/cpp \
  tests/native_actor_shared_payload_scheduler_bridge_test.cpp \
  "$workdir/native-scheduler.o" \
  -o "$workdir/shared-payload-scheduler-test"

# The test's nonzero exit is fatal. Require a nonvacuous success marker as well.
ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:abort_on_error=1' \
UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1' \
timeout 45s "$workdir/shared-payload-scheduler-test" \
  | tee "$workdir/run.log"
grep -Fxq 'NATIVE_SHARED_PAYLOAD_SCHEDULER_BRIDGE_PASS' "$workdir/run.log"
echo "EXACT_HEAD_REAL_SCHEDULER_HOST_PASS head=$head tree=$tree cc=$CC cxx=$CXX opt=O$OPT_LEVEL asan=1 ubsan=1"
