#!/usr/bin/env bash
# Production ActorArena.c host stress, not compiled .atto and not zero-GC proof.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ "$(uname -s)" == Linux ]] || { echo 'Linux-only POSIX fail-stop oracle' >&2; exit 69; }
clang="${CLANG:-clang}"
command -v "$clang" >/dev/null || { echo 'Clang unavailable' >&2; exit 69; }
for program in git timeout sha256sum; do
  command -v "$program" >/dev/null || { echo "missing $program" >&2; exit 69; }
done
source_rel="tests/actor_arena_lifecycle_stress_v1.c"
arena_rel="src/main/cpp/oreslang/runtime/ActorArena.c"
[[ -f "$root/$source_rel" && -f "$root/$arena_rel" && ! -L "$root/$source_rel" && ! -L "$root/$arena_rel" ]] || exit 78
if [[ -n "${EXPECTED_HEAD:-}" ]]; then
  [[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
  [[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
  [[ -z "$(git -C "$root" status --porcelain)" ]] || exit 78
  for rel in "$source_rel" "$arena_rel"; do
    test "$(git -C "$root" rev-parse "HEAD:$rel")" = "$(git -C "$root" hash-object --no-filters "$root/$rel")" || exit 78
    [[ "$(git -C "$root" ls-files --stage -- "$rel")" =~ ^100644[[:space:]] ]] || exit 78
  done
fi
work="$(mktemp -d)"; trap 'rm -rf -- "$work"' EXIT
test_blob="$(git -C "$root" hash-object --no-filters "$root/$source_rel")"
arena_blob="$(git -C "$root" hash-object --no-filters "$root/$arena_rel")"
for opt in 0 2; do
  "$clang" -std=c11 -O"$opt" -g -Wall -Wextra -fno-omit-frame-pointer \
    -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free \
    -fsanitize=address,undefined -pthread -I "$root/src/main/cpp" \
    "$root/$source_rel" "$root/$arena_rel" \
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free \
    -o "$work/arena-$opt"
  result="$work/result-$opt"
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout --signal=TERM 90s "$work/arena-$opt" >"$result" 2>"$work/err-$opt" || {
      cat "$work/err-$opt" >&2; echo "actor arena sanitizer O$opt failed" >&2; exit 1;
    }
  grep -Fqx 'LLVM_ACTOR_ARENA_HOST_LIFECYCLE_PASS|turns=8193|actors=2|negative=2|startup_callocs=10|physical_frees=10|turn_allocations=0|native_guest=NO|zero_gc_certified=NO' "$result" || {
    cat "$result" "$work/err-$opt" >&2; exit 1;
  }
  [[ ! -s "$work/err-$opt" ]] || { cat "$work/err-$opt" >&2; exit 1; }
  printf 'LLVM_ACTOR_ARENA_HOST_SANITIZED|O%s|test_blob=%s|runtime_blob=%s|binary_sha256=%s|guest=NOT_EXECUTED\n' \
    "$opt" "$test_blob" "$arena_blob" "$(sha256sum "$work/arena-$opt" | cut -d' ' -f1)"
done
echo 'LLVM_ACTOR_ARENA_HOST_MATRIX_PASS|compiler=clang|sanitizers=ASAN_UBSAN|levels=O0_O2|real_runtime=YES|atto_guest=NOT_EXECUTED'
