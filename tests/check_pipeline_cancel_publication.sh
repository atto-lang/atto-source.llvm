#!/usr/bin/env bash
# Regression: terminate a compiled Oreslang flow between owned wire snapshot
# and scheduler DATA publication. Linux GNU ld --wrap is deliberately used
# only inside this CTest, not by the shipping runtime.
set -euo pipefail
if [[ $# -ne 3 ]]; then
  echo "usage: $0 <oreslang-llvmc> <clang> <source-root>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
source="$3"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
"$compiler" "$source/tests/fixtures/source-actor-pipeline-two-stage.ores" > "$tmp/guest.ll"
"$clang" -O1 -g -x ir "$tmp/guest.ll" -x c -std=c11 \
  -DORESLANG_SOURCE_ACTOR_TESTING -I "$source/src/main/cpp" \
  "$source/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c" \
  "$source/src/main/cpp/oreslang/runtime/MessagePackActorV2.c" \
  "$source/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
  "$source/src/main/cpp/oreslang/runtime/NativeActorFuture.c" \
  "$source/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" \
  "$source/tests/native_pipeline_cancel_publication_race.c" \
  -pthread -lm \
  -Wl,--wrap=oreslang_source_actor_pipeline_connect_many \
  -o "$tmp/guest"
for n in $(seq 1 20); do
  set +e
  "$tmp/guest" > "$tmp/stdout" 2> "$tmp/stderr"
  exit_code=$?
  set -e
  if [[ "$exit_code" -ne 77 ]]; then
    cat "$tmp/stderr"
    echo "expected terminal cancellation exit 77; received $exit_code" >&2
    exit 1
  fi
  grep -q SOURCE_PIPELINE_CANCEL_PUBLICATION_RACE_PASS "$tmp/stderr"
  grep -q 'SOURCE_PIPELINE_TERMINAL_STATUS code=7' "$tmp/stderr"
  [[ "$(grep -c 'SOURCE_ACTOR_SPAWN_STOP_VERIFIED.*processed=0' "$tmp/stderr")" -eq 2 ]]
  grep -q 'SOURCE_ACTOR_GROUP_VERIFIED active=0' "$tmp/stderr"
  if grep -q SOURCE_ACTOR_INVARIANT_FAILURE "$tmp/stderr"; then
    cat "$tmp/stderr"
    exit 1
  fi
done
echo "NATIVE_PIPELINE_CANCELLATION_LINEARIZATION_CTEST_PASS"
