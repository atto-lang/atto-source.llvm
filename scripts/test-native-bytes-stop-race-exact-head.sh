#!/usr/bin/env bash
# Execute only inside an authorized LOCAL checkout of the actual LLVM source.
# No cross-org mirror, no standalone mock scheduler, no .atto guest claims.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
if [[ ! "${EXPECTED_LLVM_HEAD:-}" =~ ^[[:xdigit:]]{40}$ ]] ||
   [[ "$(git -C "$root" rev-parse HEAD)" != "$EXPECTED_LLVM_HEAD" ]] ||
   [[ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]]; then
  echo "NOT_EXECUTED: expected exact clean LLVM HEAD" >&2
  exit 65
fi
for tool in git gcc clang sha256sum; do
  command -v "$tool" >/dev/null ||
    { echo "NOT_EXECUTED: tool missing: $tool" >&2; exit 69; }
done
sources=(
  src/main/cpp/oreslang/runtime/NativeActorBytesInbox.h
  src/main/cpp/oreslang/runtime/NativeActorBytesInbox.c
  src/main/cpp/oreslang/runtime/NativeActorBytesSchedulerBinding.h
  src/main/cpp/oreslang/runtime/NativeActorScheduler.h
  src/main/cpp/oreslang/runtime/NativeActorScheduler.c
  tests/native_actor_bytes_atomic_stop_race_test.c
)
for path in "${sources[@]}"; do
  [[ -f "$root/$path" && ! -L "$root/$path" ]] ||
    { echo "NOT_EXECUTED: missing or symlinked $path" >&2; exit 65; }
  tracked="$(git -C "$root" rev-parse "HEAD:$path")"
  current="$(git -C "$root" hash-object -- "$root/$path")"
  [[ "$tracked" == "$current" ]] ||
    { echo "NOT_EXECUTED: Git blob mismatch $path" >&2; exit 65; }
  echo "VERIFIED_BLOB $path=$current"
done
echo "LLVM_HEAD=$(git -C "$root" rev-parse HEAD)"
echo "LLVM_TREE=$(git -C "$root" rev-parse 'HEAD^{tree}')"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
flags=( -pthread -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG
        -DORESLANG_ACTOR_BYTES_TESTING=1
        -I "$root/src/main/cpp"
        "$root/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
        "$root/src/main/cpp/oreslang/runtime/NativeActorBytesInbox.c"
        "$root/tests/native_actor_bytes_atomic_stop_race_test.c" )
# Repetitions exercise independent pthread interleavings. Each test checks
# all successful sends against DATA receipts, abandonment and scrub.
for cc in gcc clang; do
  for opt in O0 O2; do
    "$cc" "-$opt" "${flags[@]}" -o "$tmp/$cc-$opt"
    sha256sum "$tmp/$cc-$opt"
    for attempt in {1..16}; do
      "$tmp/$cc-$opt" | grep '^NATIVE_ATOMIC_BYTES_STOP_RACE_HOST_ONLY_PASS'
    done
    echo "NATIVE_BYTES_STOP_RACE_${cc}_$opt=HOST_ONLY_PASS attempts=16"
  done
done
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${flags[@]}" -o "$tmp/clang-asan-ubsan"
sha256sum "$tmp/clang-asan-ubsan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$tmp/clang-asan-ubsan" | grep '^NATIVE_ATOMIC_BYTES_STOP_RACE_HOST_ONLY_PASS'
echo "NATIVE_BYTES_STOP_RACE_ASAN_UBSAN=HOST_ONLY_PASS"
echo "ATTO_BYTES_GUEST=NOT_EXECUTED"
echo "NATIVE_KAFKA_BROKER=NOT_EXECUTED"
