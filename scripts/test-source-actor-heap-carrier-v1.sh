#!/usr/bin/env bash
# Compiler source-actor heap (ActorOwnedI64Heap.c), not ActorArena.c.
# Native host ABI evidence only: NOT compiled .atto or zero-GC certification.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?exact 40-hex source commit required}"
[[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
[[ "$(uname -s)" == Linux ]] || { echo 'Linux signal oracle required' >&2; exit 69; }
clang="${CLANG:-clang}"
clangxx="${CLANGXX:-clang++}"
for tool in "$clang" "$clangxx" git sha256sum timeout cmp; do
  command -v -- "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 69; }
done
paths=(
  src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
  src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.h
  tests/actor_owned_heap_lifecycle_test.cpp
  scripts/test-source-actor-heap-carrier-v1.sh
)
check_source() {
  [[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || return 1
  [[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || return 1
  for rel in "${paths[@]}"; do
    [[ -f "$root/$rel" && ! -L "$root/$rel" ]] || return 1
    [[ "$(git -C "$root" ls-files --stage -- "$rel")" =~ ^100644[[:space:]] ]] || return 1
    test "$(git -C "$root" rev-parse "HEAD:$rel")" = "$(git -C "$root" hash-object --no-filters "$root/$rel")" || return 1
  done
}
check_source || { echo 'SOURCE_ACTOR_HEAP_EXACT_HEAD_MISMATCH' >&2; exit 78; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
rt="$root/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"
test_cpp="$root/tests/actor_owned_heap_lifecycle_test.cpp"
tree="$(git -C "$root" rev-parse 'HEAD^{tree}')"
for opt in 0 2; do
  "$clang" -std=c11 "-O${opt}" -g -Wall -Wextra -fno-omit-frame-pointer \
    -fsanitize=address,undefined -pthread -I "$root/src/main/cpp" \
    -c "$rt" -o "$work/heap-$opt.o"
  "$clangxx" -std=c++17 "-O${opt}" -g -Wall -Wextra -fno-omit-frame-pointer \
    -fsanitize=address,undefined -pthread -I "$root/src/main/cpp" \
    "$test_cpp" "$work/heap-$opt.o" \
    -Wl,--wrap=calloc -Wl,--wrap=free -o "$work/native-$opt"
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout --signal=TERM 90s "$work/native-$opt" >"$work/stdout-$opt" 2>"$work/stderr-$opt" || {
      cat "$work/stdout-$opt" "$work/stderr-$opt" >&2
      echo "SOURCE_ACTOR_HEAP_CARRIER_FAIL|O$opt" >&2
      exit 1
    }
  printf '%s\n' 'SOURCE_ACTOR_HEAP_HOST_LIFECYCLE_OK|churn=100000|churn_turns=1000|negative=12|scrubbed_heap_frees=2|atto_guest=NO' >"$work/expected-$opt"
  if [[ -s "$work/stderr-$opt" ]] || ! cmp -s "$work/stdout-$opt" "$work/expected-$opt"; then
    cat "$work/stdout-$opt" "$work/stderr-$opt" >&2
    echo 'source actor heap did not complete all lifecycle assertions' >&2
    exit 1
  fi
  check_source || { echo 'SOURCE_ACTOR_HEAP_SOURCE_DRIFT' >&2; exit 78; }
  printf 'SOURCE_ACTOR_HEAP_CARRIER_HOST_PASS|head=%s|tree=%s|O%s|negative=12|pre_free_scrubbed=YES|sanitizers=ASAN_UBSAN|runtime_blob=%s|test_blob=%s|binary_sha256=%s|atto_guest=NO\n' \
    "$EXPECTED_HEAD" "$tree" "$opt" \
    "$(git -C "$root" rev-parse 'HEAD:src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c')" \
    "$(git -C "$root" rev-parse 'HEAD:tests/actor_owned_heap_lifecycle_test.cpp')" \
    "$(sha256sum "$work/native-$opt" | cut -d' ' -f1)"
done
echo 'SOURCE_ACTOR_HEAP_CARRIER_HOST_MATRIX_PASS|levels=O0_O2|negative_each=12|cross_carrier_stolen_token=REJECTED|migration_between_turns=ALLOWED|pre_free_scrubbed=YES|guest=NOT_EXECUTED'
