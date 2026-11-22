#!/usr/bin/env bash
# Compile real Oreslang C++ mailbox tests against the repository's headers,
# without LLVM, CMake, other organizations, checkout tokens or source mirrors.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cxx="${CXX:-c++}"
mode="${MAILBOX_SANITIZER:-address,undefined}"
repeats="${MAILBOX_TEST_REPEATS:-1}"
case "$mode" in
  address,undefined|thread|none) ;;
  *) printf 'unsupported MAILBOX_SANITIZER: %s\n' "$mode" >&2; exit 2 ;;
esac
if ! [[ "$repeats" =~ ^[1-9][0-9]{0,2}$ ]] || (( repeats > 100 )); then
  printf 'MAILBOX_TEST_REPEATS must be an integer from 1 to 100\n' >&2
  exit 2
fi
command -v "$cxx" >/dev/null || { printf 'C++ compiler not found: %s\n' "$cxx" >&2; exit 2; }
# A missing timeout utility must never permit an unbounded test run.
if command -v timeout >/dev/null 2>&1; then
  timeout_cmd=timeout
elif command -v gtimeout >/dev/null 2>&1; then
  timeout_cmd=gtimeout
else
  printf 'timeout or gtimeout is required to bound native test execution\n' >&2
  exit 2
fi

build_dir="$(mktemp -d "${TMPDIR:-/tmp}/oreslang-mailbox.XXXXXXXX")"
cleanup() {
  if [[ "${MAILBOX_KEEP_BUILD:-0}" == "1" ]]; then
    printf 'preserved test binaries: %s\n' "$build_dir"
  else
    rm -rf -- "$build_dir"
  fi
}
trap cleanup EXIT

flags=(-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer -pthread)
if [[ "$mode" != none ]]; then flags+=("-fsanitize=$mode"); fi

sources=(
  tests/bounded_actor_port_test.cpp
  tests/native_actor_port_allocation_failure_test.cpp
  tests/native_mailbox_parallel_terminal_test.cpp
  tests/priority_actor_inbox_test.cpp
  tests/actor_resource_loan_terminal_race_test.cpp
  tests/actor_mailbox_completion_model_test.cpp
  tests/native_actor_control_data_lane_test.cpp
  tests/supervisor_control_turn_test.cpp
  tests/native_actor_mailbox_completion_stress_test.cpp
  tests/native_actor_mailbox_lifecycle_test.cpp
  tests/native_actor_mailbox_randomized_model_test.cpp
)
printf 'Compiler: %s; sanitizer: %s; repeats: %s\n' "$cxx" "$mode" "$repeats"
for source in "${sources[@]}"; do
  if [[ ! -f "$root/$source" ]]; then
    printf 'missing required test source: %s\n' "$source" >&2
    exit 2
  fi
  name="${source##*/}"
  binary="$build_dir/${name%.cpp}"
  "$cxx" "${flags[@]}" -I"$root/src/main/cpp" "$root/$source" -o "$binary"
  for ((i=1; i<=repeats; ++i)); do
    "$timeout_cmd" 120s "$binary"
  done
  printf 'PASS %s (%s runs)\n' "$source" "$repeats"
done
printf 'PASS: all native mailbox targets, compiler=%s, sanitizer=%s\n' "$cxx" "$mode"
