#!/usr/bin/env bash
# Actual LLVM IR + native executable from the compiler source under test.
set -euo pipefail
if [[ $# -ne 1 ]]; then echo 'usage: native_kafka_scalar_execution.sh /absolute/oreslang-llvmc' >&2; exit 64; fi
compiler="$1"
test -x "$compiler" || exit 69
command -v clang >/dev/null || exit 69
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
positive="$root/tests/fixtures/kafka_partition_scalar_native.atto"
negative="$root/tests/fixtures/kafka_durable_actor_unsupported.atto"
test -f "$positive" && test ! -L "$positive"
test -f "$negative" && test ! -L "$negative"
echo "LLVM_KAFKA_SCALAR_BLOB=$(git -C "$root" hash-object "$positive")"
"$compiler" "$positive" > "$tmp/kafka.ll" 2> "$tmp/compile.err"
test -s "$tmp/kafka.ll" && test ! -s "$tmp/compile.err"
for opt in O0 O2; do
  clang "-$opt" -Werror -Wno-override-module -x ir "$tmp/kafka.ll" -o "$tmp/kafka-$opt"
  set +e
  "$tmp/kafka-$opt" > "$tmp/$opt.stdout" 2> "$tmp/$opt.stderr"
  result=$?
  set -e
  test "$result" -eq 42 && test ! -s "$tmp/$opt.stdout" && test ! -s "$tmp/$opt.stderr"
  echo "ATTO_KAFKA_LLVM_NATIVE_SCALAR_$opt=EXECUTED_PASS exit=42"
done

# The current LLVM compiler genuinely lowers a restricted scalar actor
# into a native MAIN-domain OresVM with a bounded FIFO and actor-owned heap.
actor="$root/tests/fixtures/kafka_partition_actor_native.atto"
test -f "$actor" && test ! -L "$actor"
echo "LLVM_KAFKA_ACTOR_BLOB=$(git -C "$root" hash-object "$actor")"
"$compiler" "$actor" > "$tmp/kafka-actor.ll" 2> "$tmp/actor-compile.err"
test -s "$tmp/kafka-actor.ll" && test ! -s "$tmp/actor-compile.err"
for symbol in oreslang_source_actor_spawn oreslang_source_actor_send oreslang_source_actor_stop oreslang_source_actor_await_ready; do
  grep -Fq "$symbol" "$tmp/kafka-actor.ll" || { echo "missing compiler-generated actor $symbol" >&2; exit 1; }
done
native="$root/src/main/cpp/oreslang/runtime"
for opt in O0 O2; do
  clang "-$opt" -x ir "$tmp/kafka-actor.ll" \
    -x c "$native/ActorOwnedI64Heap.c" "$native/NativeActorScheduler.c" \
    "$native/NativeOresVM.c" "$native/NativeActorFuture.c" \
    "$native/SourceActorBootstrap.c" "$native/NativeExecutionContextStore.c" \
    "$native/NativeContinuationStore.c" "$native/NativeAwaitTraceBridge.c" \
    "$native/NativeLogicalTrace.c" \
    -std=c11 -pthread -I "$root/src/main/cpp" \
    -DORESLANG_SOURCE_ACTOR_TESTING -o "$tmp/kafka-actor-$opt"
  set +e
  "$tmp/kafka-actor-$opt" > "$tmp/actor-$opt.stdout" 2> "$tmp/actor-$opt.stderr"
  result=$?
  set -e
  test "$result" -eq 43 && test ! -s "$tmp/actor-$opt.stdout"
  receipt="$(grep -F 'SOURCE_ACTOR_SPAWN_STOP_VERIFIED ' "$tmp/actor-$opt.stderr" || true)"
  test "$(printf '%s\n' "$receipt" | grep -c 'SOURCE_ACTOR_SPAWN_STOP_VERIFIED' || true)" -eq 1
  read -r -a fields <<< "$receipt"
  test "${fields[0]}" = 'SOURCE_ACTOR_SPAWN_STOP_VERIFIED'
  declare -A attributes=()
  for part in "${fields[@]:1}"; do
    key="${part%%=*}"; value="${part#*=}"
    case "$key" in
      constructor_on_worker|released|ready|accepted|processed|abandoned|last) ;;
      *) echo "unexpected actor receipt key $key" >&2; exit 1 ;;
    esac
    [[ "$value" =~ ^[0-9]+$ ]] || exit 1
    test -z "${attributes[$key]+present}" || exit 1
    attributes[$key]="$value"
  done
  test "${#attributes[@]}" -eq 7
  test "${attributes[released]}" -eq 1
  # The guest explicitly awaited READY; an early-stop-only receipt is invalid.
  test "${attributes[constructor_on_worker]}" -eq 1
  test "${attributes[ready]}" -eq 1
  test "${attributes[accepted]}" -eq 5
  processed="${attributes[processed]}"
  test "$processed" -ge 0 && test "$processed" -le 5
  test "${attributes[abandoned]}" -eq "$((5 - processed))"
  hashes=(0 1 13 146 1610 17715)
  test "${attributes[last]}" -eq "${hashes[$processed]}"
  # A true native scheduler continuation must resume once. It is not
  # sufficient to see a fabricated READY line or a host-only actor stub.
  test "$(grep -Fc 'SOURCE_AWAIT_READY_TRACE_PASS ' "$tmp/actor-$opt.stderr" || true)" -eq 1
  grep -Fq 'execution_id_persisted=1 fresh_resume=1 root_slot=63' "$tmp/actor-$opt.stderr"
  grep -Fq 'SOURCE_ACTOR_VM_ROUTING_VERIFIED domains=4 main=1 cold_carriers=0' "$tmp/actor-$opt.stderr"
  unset attributes
  echo "ATTO_KAFKA_LLVM_NATIVE_ACTOR_$opt=EXECUTED_PASS accepted=5 processed=$processed abandoned=$((5 - processed))"
done
echo 'ATTO_KAFKA_LLVM_NATIVE_ACTOR_SCOPE=I64_ONLY_NO_TCP_OR_FSYNC'

if "$compiler" "$negative" > "$tmp/actor.ll" 2> "$tmp/actor.stderr"; then
  echo "LLVM_KAFKA_ACTOR_UNEXPECTEDLY_COMPILED: must add runtime gate" >&2
  exit 1
fi
test ! -s "$tmp/actor.ll" && test -s "$tmp/actor.stderr"
echo 'ATTO_KAFKA_LLVM_ACTOR=UNSUPPORTED_FAIL_CLOSED'
echo 'ATTO_KAFKA_LLVM_BROKER=NOT_EXECUTED'
