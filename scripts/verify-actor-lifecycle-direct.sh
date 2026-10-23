#!/usr/bin/env bash
# Compile exact production native actor lifecycle tests without LLVM/CMake.
# Fully local: no fetch, credentials, publisher, or cross-org source transfer.
set -Eeuo pipefail
[[ $# -ge 1 && $# -le 2 && "$1" =~ ^[a-f0-9]{40}$ ]] || {
  echo "Usage: bash $0 EXPECTED_40_HEX_COMMIT [OUT_OF_TREE_BUILD_DIRECTORY]" >&2
  echo "Optional: ORES_LIFECYCLE_REPEATS=3 ORES_LIFECYCLE_ASAN=1 CC=clang CXX=clang++" >&2
  exit 2
}
expected="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
actual="$(git -C "$root" rev-parse HEAD)"
[[ "$actual" == "$expected" ]] || {
  echo "WRONG_HEAD: expected $expected actual $actual" >&2; exit 2;
}
[[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || {
  echo "DIRTY_SOURCE: refusing source with uncommitted changes" >&2; exit 2;
}
case "${ORES_LIFECYCLE_REPEATS:-3}" in
  ''|*[!0-9]*) echo "INVALID_REPEATS" >&2; exit 2;;
esac
repeats="${ORES_LIFECYCLE_REPEATS:-3}"
(( repeats >= 1 && repeats <= 100 )) || { echo "INVALID_REPEATS" >&2; exit 2; }
out="${2:-${TMPDIR:-/tmp}/oreslang-direct-lifecycle-${expected:0:12}}"
mkdir -p "$out"
out="$(cd "$out" && pwd -P)"
case "$out/" in
  "$root/"*) echo "BUILD_INSIDE_SOURCE: choose out-of-tree path" >&2; exit 2;;
esac
tree="$(git -C "$root" rev-parse 'HEAD^{tree}')"
printf 'SOURCE_HEAD=%s\nSOURCE_TREE=%s\nDIRECT_BUILD=%s\n' "$actual" "$tree" "$out" | tee "$out/source-receipt.txt"

shared_c=(
  "$root/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
  "$root/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
  "$root/src/main/cpp/oreslang/runtime/NativeOresVM.c"
  "$root/src/main/cpp/oreslang/runtime/NativeActorFuture.c"
  "$root/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c"
  "$root/src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c"
  "$root/src/main/cpp/oreslang/runtime/NativeContinuationStore.c"
  "$root/src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c"
  "$root/src/main/cpp/oreslang/runtime/NativeLogicalTrace.c"
)
tasks=(
  source_actor_implicit_child_scope
  source_actor_opaque_ref_validation
  native_actor_nested_scope_integration
  native_actor_group_managed_worker_readiness
)

run_one() {
  local mode="$1" cc="$2" cxx="$3" destination="$4"
  shift 4
  local -a flags=("$@")
  command -v "$cc" >/dev/null 2>&1 || { echo "MISSING_CC=$cc" >&2; exit 2; }
  command -v "$cxx" >/dev/null 2>&1 || { echo "MISSING_CXX=$cxx" >&2; exit 2; }
  mkdir -p "$destination"
  echo "MATRIX=$mode SOURCE_HEAD=$actual SOURCE_TREE=$tree"
  "$cc" -std=c11 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror \
    -DORESLANG_SOURCE_ACTOR_TESTING=1 \
    -I "$root/src/main/cpp" "${flags[@]}" \
    "$root/tests/source_actor_implicit_child_scope_test.c" "${shared_c[@]}" \
    -o "$destination/source_actor_implicit_child_scope"
  "$cc" -std=c11 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror \
    -I "$root/src/main/cpp" "${flags[@]}" \
    "$root/tests/source_actor_opaque_ref_validation_test.c" "${shared_c[@]}" \
    -o "$destination/source_actor_opaque_ref_validation"
  "$cxx" -std=c++17 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror \
    -I "$root/src/main/cpp" "${flags[@]}" \
    "$root/tests/native_actor_nested_scope_integration_test.cpp" \
    -o "$destination/native_actor_nested_scope_integration"
  "$cxx" -std=c++17 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror \
    -I "$root/src/main/cpp" "${flags[@]}" \
    "$root/tests/actor_group_lifecycle_test.cpp" \
    -o "$destination/native_actor_group_managed_worker_readiness"
  for name in "${tasks[@]}"; do
    for ((attempt=1; attempt<=repeats; attempt++)); do
      echo "RUN=$mode/$name ITERATION=$attempt HEAD=$actual"
      "$destination/$name"
    done
  done
  echo "MATRIX_PASS=$mode SOURCE_HEAD=$actual SOURCE_TREE=$tree"
}

run_one strict "${CC:-cc}" "${CXX:-c++}" "$out/strict" \
  2>&1 | tee "$out/strict.log"
if [[ "${ORES_LIFECYCLE_ASAN:-1}" == 1 ]]; then
  export ASAN_OPTIONS='detect_leaks=1:halt_on_error=1'
  [[ "$(uname -s)" == Darwin ]] && ASAN_OPTIONS='detect_leaks=0:halt_on_error=1'
  export UBSAN_OPTIONS='halt_on_error=1'
  run_one asan-ubsan clang clang++ "$out/asan-ubsan" \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    2>&1 | tee "$out/asan-ubsan.log"
fi
echo "EXACT_NATIVE_SOURCE_VERIFIED=$actual"
echo "RECEIPT=$out/source-receipt.txt"
