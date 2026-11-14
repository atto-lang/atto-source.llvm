#!/usr/bin/env bash
# Exact-head REAL SourceActorBootstrap.c ready-barrier host regression suite.
# Native production runtime linked with Clang ASan/UBSan at O0/O2. This is
# NOT a compiled .atto actor test and does NOT certify whole-process zero-GC.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?immutable 40-hex Git head is required}"
[[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
[[ "$(uname -s)" == Linux ]] || { echo "Linux POSIX signal oracle required" >&2; exit 69; }
clang_bin="${CLANG:-clang>"
for tool in git timeout awk grep cmp; do
  command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 69; }
done
command -v "$clang_bin" >/dev/null || { echo "missing $clang_bin" >&2; exit 69; }
ulimit -c 0 || true
paths=(
  tests/source_actor_ready_barrier_races_v1.c
  scripts/test-source-actor-ready-barrier-v1.sh
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
check_source() {
  [[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || return 1
  [[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || return 1
  for path in "${paths[@]}"; do
    [[ -f "$root/$path" && ! -L "$root/$path" ]] || return 1
    [[ "$(git -C "$root" ls-files --stage -- "$path")" =~ ^100644[[:space:]] ]] || return 1
    [[ "$(git -C "$root" hash-object --no-filters "$root/$path")" == "$(git -C "$root" rev-parse "HEAD:$path")" ]] || return 1
  done
}
check_source || { echo "SOURCE_ACTOR_READY_BARRIER_EXACT_HEAD_MISMATCH" >&2; exit 78; }
work="$(mktemp -d)"; trap 'rm -rf -- "$work"' EXIT
rt="$root/src/main/cpp/oreslang/runtime"
runtime_sources=(
  "$rt/ActorOwnedI64Heap.c" "$rt/NativeActorScheduler.c"
  "$rt/NativeOresVM.c" "$rt/NativeActorFuture.c"
  "$rt/SourceActorBootstrap.c" "$rt/NativeExecutionContextStore.c"
  "$rt/NativeContinuationStore.c" "$rt/NativeAwaitTraceBridge.c"
  "$rt/NativeLogicalTrace.c"
)
for opt in 0 2; do
  bin="$work/source-actor-ready-barrier-O$opt"
  "$clang_bin" -std=c11 "-O$opt" -g -Wall -Wextra \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -DORESLANG_SOURCE_ACTOR_TESTING -I "$root/src/main/cpp" \
    "$root/tests/source_actor_ready_barrier_races_v1.c" \
    "${runtime_sources[@]}" -pthread -o "$bin"

  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
    timeout 90s "$bin" positive >"$work/positive.out" 2>"$work/positive.err" || {
      cat "$work/positive.out" "$work/positive.err" >&2
      echo "SOURCE_ACTOR_READY_BARRIER_POSITIVE_FAILED|O$opt" >&2
      exit 1
    }
  printf '%s\n' 'SOURCE_ACTOR_READY_RACE_POSITIVE|constructors=40|cv_waits=32|fast_ready=8|joins=40' >"$work/positive.expected"
  cmp -s "$work/positive.expected" "$work/positive.out" || {
    cat "$work/positive.out" >&2; exit 1;
  }
  awk '
    /^SOURCE_ACTOR_VM_ROUTING_VERIFIED / {
      if ($0 != "SOURCE_ACTOR_VM_ROUTING_VERIFIED domains=4 main=1 cold_carriers=0 root_slot=63") bad=1
      routing++; next
    }
    /^SOURCE_ACTOR_SPAWN_STOP_VERIFIED / {
      if ($0 != "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 ready=1 accepted=0 processed=0 abandoned=0 last=0") bad=1
      stop++; next
    }
    /^SOURCE_ACTOR_GROUP_VERIFIED / {
      if ($0 != "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=1 peak=1 carriers=4") bad=1
      group++; next
    }
    { bad=1 }
    END { exit (routing==40 && stop==40 && group==40 && !bad ? 0 : 1) }
  ' "$work/positive.err" || {
    echo "SOURCE_ACTOR_READY_BARRIER_NOT_40_PHYSICAL_JOINS|O$opt" >&2
    cat "$work/positive.err" >&2
    exit 1
  }

  for mode in cancel-pending cancel-publishing carrier; do
    case "$mode" in
      cancel-pending|cancel-publishing) marker="SOURCE_ACTOR_READY_BARRIER_CANCELLED" ;;
      carrier) marker="SOURCE_ACTOR_READY_BARRIER_CARRIER_DENIED" ;;
    esac
    rc=0
    ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
      timeout 45s "$bin" "$mode" >"$work/$mode.out" 2>"$work/$mode.err" || rc=$?
    if [[ "$rc" != 134 || -s "$work/$mode.out" ]] ||
       ! grep -Fqx "$marker" "$work/$mode.err"; then
      echo "SOURCE_ACTOR_READY_BARRIER_FAIL_STOP_NOT_PROVEN|O$opt|mode=$mode|exit=$rc" >&2
      cat "$work/$mode.out" "$work/$mode.err" >&2
      exit 1
    fi
    if grep -Eq 'ERROR: AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer|LeakSanitizer|SOURCE_ACTOR_READY_RACE_INVARIANT_FAILED' "$work/$mode.err"; then
      echo "SOURCE_ACTOR_READY_BARRIER_UNEXPECTED_SANITIZER_FAILURE|O$opt|mode=$mode" >&2
      cat "$work/$mode.err" >&2
      exit 1
    fi
  done
  check_source || { echo "SOURCE_ACTOR_READY_BARRIER_SOURCE_DRIFT" >&2; exit 78; }
  printf 'SOURCE_ACTOR_READY_BARRIER_NATIVE_PASS|head=%s|O%s|constructors=40|cv_waits=32|fast_ready=8|physical_joins=40|cancel_pending=SIGABRT|cancel_publishing=SIGABRT|carrier=SIGABRT|sanitizers=ASAN_UBSAN|atto_guest=NO|zero_gc_certified=NO\n' \
    "$EXPECTED_HEAD" "$opt"
done
echo "SOURCE_ACTOR_READY_BARRIER_NATIVE_MATRIX_PASS|head=$EXPECTED_HEAD|levels=O0_O2|native_execs=8|real_ctors=80|real_joins=80|cv_waits=64|fast_paths=16|death_tests=6|guest=NOT_EXECUTED"
