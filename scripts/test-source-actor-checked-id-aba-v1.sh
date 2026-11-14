#!/usr/bin/env bash
# Real production actor allocator-address ABA test. This does not emulate
# actors or claim compiler-generated .atto Future continuation parity.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?immutable exact source head SHA required}"
[[ "$EXPECTED_HEAD" =~ ^[a-f0-9]{40}$ ]] || exit 64
[[ "$(uname -s)" == Linux ]] || exit 69
clang_bin="${CLANG:-clang>"
for tool in git timeout cmp grep awk sha256sum; do
  command -v "$tool" >/dev/null || exit 69
done
command -v "$clang_bin" >/dev/null || exit 69
ulimit -c 0 || true
paths=(
  tests/source_actor_checked_identity_aba_v1.c
  scripts/test-source-actor-checked-id-aba-v1.sh
  src/main/cpp/oreslang/runtime/SourceActorBootstrap.c
  src/main/cpp/oreslang/runtime/SourceActorBootstrap.h
  src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c
  src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.h
  src/main/cpp/oreslang/runtime/NativeActorScheduler.c
  src/main/cpp/oreslang/runtime/NativeOresVM.c
  src/main/cpp/oreslang/runtime/NativeActorFuture.c
  src/main/cpp/oreslang/runtime/NativeExecutionContextStore.c
  src/main/cpp/oreslang/runtime/NativeContinuationStore.c
  src/main/cpp/oreslang/runtime/NativeAwaitTraceBridge.c
  src/main/cpp/oreslang/runtime/NativeLogicalTrace.c
)
check_snapshot() {
  [[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || return 1
  [[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || return 1
  for path in "${paths[@]}"; do
    [[ -f "$root/$path" && ! -L "$root/$path" ]] || return 1
    [[ "$(git -C "$root" ls-files --stage -- "$path")" =~ ^100644[[:space:]] ]] || return 1
    [[ "$(git -C "$root" hash-object --no-filters "$root/$path")" == "$(git -C "$root" rev-parse "HEAD:$path")" ]] || return 1
  done
}
check_snapshot || { echo "SOURCE_ACTOR_CHECKED_ID_SOURCE_MISMATCH" >&2; exit 78; }
rt="$root/src/main/cpp/oreslang/runtime"
sources=(
  "$rt/ActorOwnedI64Heap.c" "$rt/NativeActorScheduler.c"
  "$rt/NativeOresVM.c" "$rt/NativeActorFuture.c"
  "$rt/SourceActorBootstrap.c" "$rt/NativeExecutionContextStore.c"
  "$rt/NativeContinuationStore.c" "$rt/NativeAwaitTraceBridge.c"
  "$rt/NativeLogicalTrace.c"
)
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
for opt in 0 2; do
  bin="$work/checked-id-aba-O$opt"
  "$clang_bin" -std=c11 "-O$opt" -g -Wall -Wextra \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -DORESLANG_SOURCE_ACTOR_TESTING -I "$root/src/main/cpp" \
    "$root/tests/source_actor_checked_identity_aba_v1.c" \
    "${sources[@]}" -pthread -o "$bin"
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 120s "$bin" positive >"$work/pos.out" 2>"$work/pos.err" || {
      echo "SOURCE_ACTOR_CHECKED_ID_POSITIVE_FAILED|O$opt" >&2
      cat "$work/pos.out" "$work/pos.err" >&2
      exit 1
    }
  printf '%s\n' 'SOURCE_ACTOR_CHECKED_ID_ABA_PASS|actors=128|actual_address_reuses=127|stale_status=128|stale_send=128|stale_stop=128|physical_joins=128' >"$work/expected"
  cmp -s "$work/expected" "$work/pos.out" || {
    cat "$work/pos.out" >&2
    exit 1
  }
  awk '
    /^SOURCE_ACTOR_VM_ROUTING_VERIFIED / { routing++; next }
    /^SOURCE_ACTOR_SPAWN_STOP_VERIFIED / {
      if ($0 !~ /constructor_on_worker=1 released=1 ready=1 accepted=1 /) bad=1
      stop++; next
    }
    /^SOURCE_ACTOR_GROUP_VERIFIED / {
      if ($0 != "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=1 peak=1 carriers=4") bad=1
      group++; next
    }
    { bad=1 }
    END { exit(routing==128 && stop==128 && group==128 && !bad ? 0 : 1) }
  ' "$work/pos.err" || {
    echo "SOURCE_ACTOR_CHECKED_ID_MISSING_PHYSICAL_EVIDENCE|O$opt" >&2
    cat "$work/pos.err" >&2
    exit 1
  }
  for mode in stale-send stale-stop stale-join; do
    rc=0
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
      timeout 45s "$bin" "$mode" >"$work/$mode.out" 2>"$work/$mode.err" || rc=$?
    if [[ "$rc" != 134 || -s "$work/$mode.out" ]] ||
       ! grep -Fqx 'SOURCE_ACTOR_CHECKED_ID_REJECTED' "$work/$mode.err"; then
      echo "SOURCE_ACTOR_CHECKED_ID_INVALID_ACCEPTED|O$opt|mode=$mode|exit=$rc" >&2
      cat "$work/$mode.out" "$work/$mode.err" >&2
      exit 1
    fi
    if grep -Eq 'ERROR: AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer|LeakSanitizer|SOURCE_ACTOR_CHECKED_ID_INVARIANT_FAILED' "$work/$mode.err"; then
      echo "SOURCE_ACTOR_CHECKED_ID_UNEXPECTED_SANITIZER_FAILURE|O$opt|mode=$mode" >&2
      cat "$work/$mode.err" >&2
      exit 1
    fi
  done
  check_snapshot || { echo "SOURCE_ACTOR_CHECKED_ID_SOURCE_DRIFT" >&2; exit 78; }
  printf 'SOURCE_ACTOR_CHECKED_ID_NATIVE_PASS|head=%s|O%s|actors=128|same_address_reuses=127|negative=3|asan_ubsan=EXECUTED|native_sha256=%s|atto_guest=NO\n' \
    "$EXPECTED_HEAD" "$opt" "$(sha256sum "$bin" | cut -d' ' -f1)"
done
echo "SOURCE_ACTOR_CHECKED_ID_NATIVE_MATRIX_PASS|head=$EXPECTED_HEAD|levels=O0_O2|real_actors=256|forced_address_reuses=254|death_tests=6|checked_id=64_BIT_VALUE|legacy_pointer_ABA=OPEN|async_await=NOT_EXECUTED"
