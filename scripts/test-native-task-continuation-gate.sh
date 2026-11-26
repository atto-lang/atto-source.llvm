#!/usr/bin/env bash
# Standalone native Task/continuation gate tests from an exact source checkout.
# No external CI tokens, Maven, LLVM installation, or mirror credentials.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

mode="${1:-normal}"
case "$mode" in
  normal|sanitize) ;;
  *) echo "usage: $0 [normal|sanitize]" >&2; exit 2 ;;
esac

: "${CC:=cc}"
: "${CXX:=c++}"

build_dir="$(mktemp -d "${TMPDIR:-/tmp}/oreslang-native-task-gate.XXXXXX")"
trap 'rm -rf -- "$build_dir"' EXIT

common=(-g -O1 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
if [[ "$mode" == sanitize ]]; then
  common+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
include=(-Isrc/main/cpp)
sources=(
  NativeExecutionContextStore
  NativeContinuationStore
  NativeAwaitTraceBridge
)
objects=()
for source in "${sources[@]}"; do
  "$CC" -std=c11 "${common[@]}" "${include[@]}" \
    -c "src/main/cpp/oreslang/runtime/${source}.c" \
    -o "$build_dir/$source.o"
  objects+=("$build_dir/$source.o")
done

"$CXX" -std=c++17 "${common[@]}" "${include[@]}" \
  tests/native_task_cancellation_test.cpp \
  -o "$build_dir/task-cancellation"

"$CXX" -std=c++17 "${common[@]}" "${include[@]}" \
  tests/native_task_continuation_gate_test.cpp "${objects[@]}" \
  -o "$build_dir/task-continuation"

"$CXX" -std=c++17 "${common[@]}" "${include[@]}" \
  tests/native_structured_task_scope_gate_test.cpp \
  -o "$build_dir/structured-task-scope"

"$CC" -std=c11 "${common[@]}" "${include[@]}" \
  tests/native_continuation_ready_resume_test.c "${objects[@]}" \
  -o "$build_dir/continuation-ready"

"$build_dir/task-cancellation"
"$build_dir/task-continuation"
"$build_dir/structured-task-scope"
"$build_dir/continuation-ready"

printf 'NATIVE_TASK_GATE_TESTS_PASS mode=%s CC=%s CXX=%s\n' "$mode" "$CC" "$CXX"
