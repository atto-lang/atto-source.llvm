#!/usr/bin/env bash
set -euo pipefail

# Runs the exact production NativeActorRetirementBarrier + ActorOwnedI64Heap
# test without installing the LLVM SDK. Host/native evidence ONLY, never a
# compiled .atto full-AOT or process no-GC certification.
#
# Must run inside an approved local source checkout. No source is exported,
# no network is accessed, and this script never mirrors private Git objects.
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$repo"
actual_head="$(git rev-parse --verify HEAD)"
if [[ -n "${EXPECTED_HEAD:-}" && "$actual_head" != "$EXPECTED_HEAD" ]]; then
  printf 'FAIL: source HEAD mismatch: expected=%s actual=%s\n' "$EXPECTED_HEAD" "$actual_head" >&2
  exit 2
fi
printf 'ATTO_NATIVE_RETIREMENT_SOURCE_HEAD=%s\n' "$actual_head"

for exe in cmake ctest clang clang++ gcc g++; do
  command -v "$exe" >/dev/null || {
    printf 'FAIL: missing required compiler/test executable: %s\n' "$exe" >&2
    exit 3
  }
done

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
for suite in clang gcc; do
  if [[ "$suite" == clang ]]; then
    cc=clang
    cxx=clang++
  else
    cc=gcc
    cxx=g++
  fi

  build="$tmp/build-$suite"
  printf 'ATTO_NATIVE_RETIREMENT_COMPILER=%s\n' "$("$cxx" --version | head -n 1)"
  CC="$cc" CXX="$cxx" cmake -S "$repo" -B "$build" \
    -DATTO_ACTOR_RETIREMENT_HOST_ONLY=ON \
    -DBUILD_TESTING=ON \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
    -DCMAKE_CXX_FLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build "$build" --parallel 2
  suites="^(native_actor_done_resource_quiescence|native_actor_resource_registry_quiescence|native_actor_group_managed_worker_readiness)$"
  # Fail if any of the THREE expected production test targets is missing.
  ctest --test-dir "$build" -N -R "$suites" | grep -F "Total Tests: 3"
  # Retirement integration closes successful actors: leak checking stays ON.
  ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    ctest --test-dir "$build" -R "^native_actor_done_resource_quiescence$" \
      --output-on-failure --no-tests=error
  # Group lifecycle must not strand stop dispatch or acknowledge DONE while
  # a trusted STOP callback remains on a foreign carrier. No intentional
  # long-lived resource captures are left by this group-only regression.
  ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    ctest --test-dir "$build" -R "^native_actor_group_managed_worker_readiness$" \
      --output-on-failure --no-tests=error
  # Registry failure fixtures intentionally retain unsafe FFI captures until
  # verified isolate shutdown. ASan+UBSan remain on, LSAN is scoped off here.
  ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    ctest --test-dir "$build" -R "^native_actor_resource_registry_quiescence$" \
      --output-on-failure --no-tests=error
  printf "ATTO_NATIVE_RETIREMENT_GROUP_LSAN_AND_REGISTRY_ASAN_UBSAN_PASS=%s %s\n" "$suite" "$actual_head"
done
