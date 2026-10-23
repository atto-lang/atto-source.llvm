#!/usr/bin/env bash
# Exact-commit local verification of production OresVM actor lifecycle tests.
# No checkout, upload, credential, network, or cross-org mirror operation.
set -Eeuo pipefail

usage() {
  echo "Usage: $0 EXPECTED_40_HEX_COMMIT [BUILD_DIRECTORY]" >&2
  echo "Optional: ORES_LIFECYCLE_REPEATS=5 ORES_LIFECYCLE_ASAN=1 LLVM_DIR=/path/to/llvm/lib/cmake/llvm" >&2
}
[[ $# -ge 1 && $# -le 2 ]] || { usage; exit 2; }
expected="$1"
[[ "$expected" =~ ^[a-f0-9]{40}$ ]] || { usage; exit 2; }

for tool in git cmake ctest python3; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "MISSING_TOOL: $tool" >&2; exit 2;
  }
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
root="$(cd "$script_dir/.." && pwd -P)"
actual_root="$(cd "$root" && git rev-parse --show-toplevel)"
[[ "$actual_root" == "$root" ]] || {
  echo "WRONG_CHECKOUT: expected repository root $root, got $actual_root" >&2
  exit 2
}
actual="$(git -C "$root" rev-parse HEAD)"
[[ "$actual" == "$expected" ]] || {
  echo "WRONG_HEAD: expected $expected, actual $actual" >&2
  exit 2
}
[[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || {
  echo "DIRTY_CHECKOUT: refusing uncommitted test inputs" >&2
  exit 2
}

case "${ORES_LIFECYCLE_REPEATS:-3}" in
  ''|*[!0-9]*) echo "INVALID_REPEATS: positive integer required" >&2; exit 2;;
esac
repeats="${ORES_LIFECYCLE_REPEATS:-3}"
(( repeats >= 1 && repeats <= 100 )) || {
  echo "INVALID_REPEATS: allowed 1..100" >&2; exit 2;
}

tree="$(git -C "$root" rev-parse 'HEAD^{tree}')"
build_dir="${2:-${TMPDIR:-/tmp}/oreslang-lifecycle-${expected:0:12}}"
mkdir -p "$build_dir"
build_dir="$(cd "$build_dir" && pwd -P)"
case "$build_dir/" in
  "$root/"*) echo "BUILD_INSIDE_SOURCE: use an out-of-tree build" >&2; exit 2;;
esac

# Freeze the exact inputs in a local receipt; no remote publisher or secrets.
printf 'SOURCE_HEAD=%s\nSOURCE_TREE=%s\nBUILD_DIRECTORY=%s\n' \
  "$actual" "$tree" "$build_dir" | tee "$build_dir/source-receipt.txt"
targets=(
  oreslang-source-actor-implicit-child-scope-test
  oreslang-source-actor-opaque-ref-validation-test
  oreslang-native-nested-scope-integration-test
  oreslang-actor-group-lifecycle-test
)
names=(
  source_actor_implicit_child_scope
  source_actor_opaque_ref_validation
  native_actor_nested_scope_integration
  native_actor_group_managed_worker_readiness
)
test_regex='^(source_actor_implicit_child_scope|source_actor_opaque_ref_validation|native_actor_nested_scope_integration|native_actor_group_managed_worker_readiness)$'

run_matrix() {
  local mode="$1" target_build="$2"
  shift 2
  local -a cmake_args=("$@")
  if [[ -n "${LLVM_DIR:-}" ]]; then
    cmake_args+=("-DLLVM_DIR=$LLVM_DIR")
  fi
  echo "BUILD_MATRIX=$mode SOURCE_HEAD=$actual SOURCE_TREE=$tree"
  cmake -S "$root" -B "$target_build" -DBUILD_TESTING=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo "${cmake_args[@]}"
  cmake --build "$target_build" --parallel 2 --target "${targets[@]}"
  # Fail closed if CMake silently drops one of our exact target test names.
  ctest --test-dir "$target_build" --show-only=json-v1 |
    python3 -c 'import json,sys
required=set(sys.argv[1:])
actual={x["name"] for x in json.load(sys.stdin).get("tests",[])}
missing=sorted(required-actual)
if missing:
    sys.exit("MISSING_CTEST_TARGETS: "+", ".join(missing))
print("EXACT_CTEST_TARGETS_VERIFIED:", ", ".join(sorted(required)))' "${names[@]}"
  ctest --test-dir "$target_build" -R "$test_regex" --output-on-failure \
    --timeout 120 --repeat "until-fail:$repeats"
  echo "MATRIX_PASS=$mode SOURCE_HEAD=$actual SOURCE_TREE=$tree"
}

run_matrix standard "$build_dir/standard" 2>&1 | tee "$build_dir/standard.log"
if [[ "${ORES_LIFECYCLE_ASAN:-1}" == 1 ]]; then
  command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1 || {
    echo "MISSING_TOOL: clang and clang++ required for ASan/UBSan" >&2
    exit 2
  }
  asan_options='detect_leaks=1:halt_on_error=1'
  [[ "$(uname -s)" == Darwin ]] && asan_options='detect_leaks=0:halt_on_error=1'
  export ASAN_OPTIONS="$asan_options" UBSAN_OPTIONS='halt_on_error=1'
  run_matrix asan-ubsan "$build_dir/asan-ubsan" \
    -DCMAKE_C_COMPILER="$(command -v clang)" \
    -DCMAKE_CXX_COMPILER="$(command -v clang++)" \
    -DCMAKE_C_FLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' \
    -DCMAKE_CXX_FLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' \
    -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
    2>&1 | tee "$build_dir/asan-ubsan.log"
fi
echo "LIFECYCLE_EXACT_SOURCE_VERIFIED=$actual"
echo "RECEIPT=$build_dir/source-receipt.txt"
