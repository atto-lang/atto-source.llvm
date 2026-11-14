#!/usr/bin/env bash
# Exact-head compiled native .atto checked-ID ABI conformance.
# No synthetic output. Fail if LLVM emits pointer API calls in a non-await guest.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?immutable SHA required}"
: "${ORESLANG_LLVMC:?compile exact-head native compiler first}"
: "${ORES_NO_GC_IR_AUDIT:?compile exact-head no-GC IR gate first}"
clang_bin="${CLANG:-clang}"
[[ "$EXPECTED_HEAD" =~ ^[a-f0-9]{40}$ ]] || exit 64
for cmd in git timeout grep cmp sha256sum; do
  command -v "$cmd" >/dev/null || exit 69
done
command -v "$clang_bin" >/dev/null || exit 69
work="$(mktemp -d)"; trap 'rm -rf -- "$work"' EXIT
sources=(
  tests/fixtures/source-actor-checked-id-v1/single.atto
  tests/fixtures/source-actor-checked-id-v1/five.atto
  tests/fixtures/source-actor-checked-id-v1/await-ready.atto
  tests/fixtures/source-actor-checked-id-v1/await-done.atto
  tests/fixtures/source-actor-checked-id-v1/escape.atto
  tests/fixtures/source-actor-checked-id-v1/abi-shadow.atto
)
check_source() {
  [[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || return 1
  [[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || return 1
  for rel in "${sources[@]}"; do
    [[ -f "$root/$rel" && ! -L "$root/$rel" ]] || return 1
    [[ "$(git -C "$root" hash-object --no-filters "$root/$rel")" == "$(git -C "$root" rev-parse "HEAD:$rel")" ]] || return 1
  done
}
check_source || { echo "SOURCE_ACTOR_CHECKED_GUEST_SOURCE_DRIFT" >&2; exit 78; }
compiler="$(realpath -e "$ORESLANG_LLVMC")"
auditor="$(realpath -e "$ORES_NO_GC_IR_AUDIT")"
[[ -x "$compiler" && -x "$auditor" ]] || exit 69
for opt in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  for variant in single five await-ready await-done; do
    rel="tests/fixtures/source-actor-checked-id-v1/$variant.atto"
    timeout 120s "$compiler" --target=host "$root/$rel" >"$work/program.ll" 2>"$work/compiler.err" || {
      cat "$work/compiler.err" >&2
      echo "SOURCE_ACTOR_CHECKED_GUEST_NOT_COMPILED|O$opt|case=$variant" >&2
      exit 1
    }
    [[ -s "$work/program.ll" && ! -s "$work/compiler.err" ]] || exit 1
    "$auditor" "$work/program.ll" >"$work/gc.log"
    case "$variant" in
      single|five) checked_apis=(spawn_id send_id stop_id) ;;
      await-ready) checked_apis=(spawn_id send_id stop_id await_ready_id) ;;
      await-done) checked_apis=(spawn_id request_stop_id await_done_id) ;;
      *) echo "unreviewed guest variant: $variant" >&2; exit 78 ;;
    esac
    for api in "${checked_apis[@]}"; do
      if ! grep -Eq "[[:space:]]call[[:space:]].*@oreslang_source_actor_$api[(]" "$work/program.ll"; then
        echo "SOURCE_ACTOR_CHECKED_GUEST_MISSING_TYPED_CALL|O$opt|case=$variant|api=$api" >&2
        exit 1
      fi
    done
    # Reject *any* generated legacy malloc-address runtime invocation,
    # including the legacy Future entry points: a checked callback must
    # always take a 64-bit actor identity, never a void* ActorRef.
    if grep -Eq '[[:space:]]call[[:space:]].*@oreslang_source_actor_(spawn|send|stop|request_stop|await_ready|await_done)[(]' "$work/program.ll"; then
      echo "SOURCE_ACTOR_CHECKED_GUEST_LEGACY_PTR_CALL|O$opt|case=$variant" >&2
      exit 1
    fi
    binary="$work/guest-$opt-$variant"
    timeout 120s "$root/scripts/compile-aot.sh" "$root/$rel" "$binary" >"$work/build.log"
    [[ -f "$binary" && -x "$binary" && ! -L "$binary" ]] || exit 1
    for run in 1 2 3 4; do
      rc=0
      timeout 45s "$binary" >"$work/out" 2>"$work/err" || rc=$?
      if [[ "$rc" != 43 || -s "$work/out" || -s "$work/err" ]]; then
        echo "SOURCE_ACTOR_CHECKED_GUEST_NATIVE_FAIL|O$opt|case=$variant|run=$run|exit=$rc" >&2
        cat "$work/out" "$work/err" >&2
        exit 1
      fi
    done
    printf 'SOURCE_ACTOR_CHECKED_GUEST_EXECUTED|head=%s|O%s|case=%s|runs=4|native_exit=43|legacy_pointer_calls=0|sha256=%s\n' \
      "$EXPECTED_HEAD" "$opt" "$variant" "$(sha256sum "$binary" | cut -d' ' -f1)"
  done
  for negative in escape abi-shadow; do
    case "$negative" in
      escape) expected_error='ActorRef|actor.*ref|source actor|cannot.*return|expected.*int' ;;
      abi-shadow) expected_error='reserved native checked source actor ABI symbol collision' ;;
    esac
    rc=0
    timeout 30s "$compiler" --target=host \
      "$root/tests/fixtures/source-actor-checked-id-v1/$negative.atto" \
      >"$work/negative.ll" 2>"$work/negative.err" || rc=$?
    if [[ "$rc" == 0 || "$rc" == 124 || -s "$work/negative.ll" ]] ||
         ! grep -Eiq "$expected_error" "$work/negative.err"; then
      echo "SOURCE_ACTOR_CHECKED_GUEST_INVALID_ACCEPTED|O$opt|fixture=$negative|exit=$rc" >&2
      cat "$work/negative.err" >&2
      exit 1
    fi
  done
  check_source || { echo "SOURCE_ACTOR_CHECKED_GUEST_SOURCE_DRIFT" >&2; exit 78; }
done
echo "SOURCE_ACTOR_CHECKED_GUEST_MATRIX_EXECUTED|head=$EXPECTED_HEAD|O0_O2=1|native_runs=32|compiled_cases=8|negative_rejections=4|ready_done_id_callbacks=2_PER_OPT|legacy_pointer_calls=0|linked_zero_gc=NOT_CERTIFIED"
