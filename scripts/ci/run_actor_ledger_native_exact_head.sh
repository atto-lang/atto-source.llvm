#!/usr/bin/env bash
# Run native actor regression tests in an already-authorized local checkout.
# No network, GitHub token, source mirror, or cross-repository operation.
set -euo pipefail

if [[ $# -ne 1 || ! "$1" =~ ^[a-f0-9]{40}$ ]]; then
  printf 'Usage: %s <exact-40-character-git-head-sha>\n' "$0" >&2
  exit 64
fi
expected_sha=$1
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
cd "$root"

actual_sha=$(git rev-parse --verify 'HEAD^{commit}')
if [[ "$actual_sha" != "$expected_sha" ]]; then
  printf 'REFUSAL: HEAD=%s does not match expected=%s\n' "$actual_sha" "$expected_sha" >&2
  exit 65
fi
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
  echo 'REFUSAL: exact-head test requires a clean worktree' >&2
  exit 65
fi

source_paths=(
  src/main/cpp/oreslang/runtime/NativeActorScheduler.c
  src/main/cpp/oreslang/runtime/NativeActorScheduler.h
  src/main/cpp/oreslang/runtime/ActorExecutionLedger.hpp
  src/main/cpp/oreslang/runtime/ActorLedgerLeaseAdapter.hpp
  src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
  src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.h
  tests/native_actor_ledger_scheduler_bridge_test.cpp
  tests/native_actor_ledger_scheduler_stop_cleanup_test.cpp
  tests/actor_owned_heap_lifecycle_test.cpp
  tests/pinned_actor_heap_suspend_test.cpp
  tests/native_actor_retained_cleanup_autoadmit_test.c
  tests/test_actor_ledger_workflow_inputs.py
  CMakeLists.txt
  .github/workflows/actor-execution-ledger-exact-head.yml
)
for path in "${source_paths[@]}"; do
  [[ -f "$path" && ! -L "$path" ]] || { echo "REFUSAL: missing/symlink input: $path" >&2; exit 65; }
  actual_blob=$(git hash-object -- "$path")
  expected_blob=$(git rev-parse --verify "HEAD:$path")
  [[ "$actual_blob" == "$expected_blob" ]] || { echo "REFUSAL: blob changed: $path" >&2; exit 65; }
done

# This audits the entire workflow input closure, not merely the above shortlist.
python3 -B tests/test_actor_ledger_workflow_inputs.py
for tool in gcc g++ clang clang++; do command -v "$tool" >/dev/null; done
command -v timeout >/dev/null || { echo 'REFUSAL: GNU timeout required' >&2; exit 69; }
work=$(mktemp -d "${TMPDIR:-/tmp}/ores-ledger-native.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT

run_executable() {
  local name=$1
  local executable=$2
  timeout 60s "$executable"
  printf 'NATIVE_ACTOR_EXECUTED compiler=%s test=%s sha=%s\n' "$current_compiler" "$name" "$expected_sha"
}

for current_compiler in gcc clang clang-sanitized; do
  case "$current_compiler" in
    gcc) cc=gcc; cxx=g++; opts=(-O2); san=(); suffix=gcc ;;
    clang) cc=clang; cxx=clang++; opts=(-O2); san=(); suffix=clang ;;
    clang-sanitized) cc=clang; cxx=clang++; opts=(-O1 -g); san=(-fsanitize=address,undefined -fno-omit-frame-pointer); suffix=san ;;
  esac
  "$cc" -std=c11 "${opts[@]}" -UNDEBUG -Wall -Wextra -Wpedantic -Werror \
    -pthread "${san[@]}" -Isrc/main/cpp -c \
    src/main/cpp/oreslang/runtime/NativeActorScheduler.c -o "$work/scheduler-$suffix.o"
  "$cc" -std=c11 "${opts[@]}" -UNDEBUG -Wall -Wextra -Wpedantic -Werror \
    -pthread "${san[@]}" -Isrc/main/cpp -c \
    src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c -o "$work/heap-$suffix.o"
  for test in native_actor_ledger_scheduler_bridge_test native_actor_ledger_scheduler_stop_cleanup_test; do
    "$cxx" -std=c++17 "${opts[@]}" -UNDEBUG -Wall -Wextra -Wpedantic -Werror \
      -pthread "${san[@]}" -Isrc/main/cpp "tests/$test.cpp" \
      "$work/scheduler-$suffix.o" "$work/heap-$suffix.o" -o "$work/$test-$suffix"
    run_executable "$test" "$work/$test-$suffix"
  done
  for test in actor_owned_heap_lifecycle_test pinned_actor_heap_suspend_test; do
    "$cxx" -std=c++17 "${opts[@]}" -UNDEBUG -Wall -Wextra -Wpedantic -Werror \
      -pthread "${san[@]}" -Isrc/main/cpp "tests/$test.cpp" \
      "$work/heap-$suffix.o" -o "$work/$test-$suffix"
    run_executable "$test" "$work/$test-$suffix"
  done
  # Sanitized success path retains leak detection. The non-sanitized
  # quarantine test intentionally holds failed-cleanup memory until shutdown.
  cleanup_defines=()
  if [[ "$current_compiler" == clang-sanitized ]]; then
    cleanup_defines=(-DORES_NATIVE_CLEANUP_SUCCESS_ONLY=1)
  fi
  "$cc" -std=c11 "${opts[@]}" -UNDEBUG -Wall -Wextra -Wpedantic -Werror \
    "${cleanup_defines[@]}" -pthread "${san[@]}" -Isrc/main/cpp \
    tests/native_actor_retained_cleanup_autoadmit_test.c \
    "$work/scheduler-$suffix.o" -o "$work/cleanup-$suffix"
  run_executable retained_cleanup "$work/cleanup-$suffix"
done
printf 'NATIVE_ACTOR_EXACT_HEAD_HOST_MATRIX_PASS sha=%s configurations=3 tests_each=5\n' "$expected_sha"
