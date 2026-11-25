#!/usr/bin/env bash
# Run only in an authorized local/private source checkout. No cross-org copy.
# Host-native substrate; NEVER count as compiled .atto guest or Kafka proof.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
if [[ ! "${EXPECTED_LLVM_HEAD:-}" =~ ^[0-9a-f]{40}$ ]] ||
   [[ "$(git -C "$root" rev-parse HEAD)" != "$EXPECTED_LLVM_HEAD" ]] ||
   [[ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]]; then
  echo "NOT_EXECUTED: exact clean LLVM source HEAD required" >&2
  exit 65
fi
for tool in git clang gcc; do
  command -v "$tool" >/dev/null ||
    { echo "NOT_EXECUTED: missing $tool" >&2; exit 69; }
done
sources=(
  src/main/cpp/oreslang/runtime/NativeActorScheduler.c
  src/main/cpp/oreslang/runtime/NativeActorScheduler.h
  src/main/cpp/oreslang/runtime/NativeActorFuture.c
  src/main/cpp/oreslang/runtime/NativeOresVM.c
  src/main/cpp/oreslang/runtime/NativeOresVM.h
  src/main/cpp/oreslang/runtime/NativeActorBytesInbox.c
  src/main/cpp/oreslang/runtime/NativeActorBytesInbox.h
  src/main/cpp/oreslang/runtime/NativeActorBytesSchedulerBinding.h
  src/main/cpp/oreslang/runtime/NativeOresVMBytesOwned.h
  tests/native_vm_owned_bytes_lifecycle_test.c
)
for p in "${sources[@]}"; do
  [[ -f "$root/$p" && ! -L "$root/$p" ]] ||
    { echo "NOT_EXECUTED: absent/symlinked $p" >&2; exit 65; }
  expected="$(git -C "$root" rev-parse "HEAD:$p")"
  observed="$(git -C "$root" hash-object -- "$root/$p")"
  [[ "$expected" == "$observed" ]] ||
    { echo "NOT_EXECUTED: changed source $p" >&2; exit 65; }
  echo "VERIFIED_BLOB $p=$observed"
done
echo "SOURCE_HEAD=$EXPECTED_LLVM_HEAD"
echo "SOURCE_TREE=$(git -C "$root" rev-parse 'HEAD^{tree}')"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
args=( -pthread -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG
       -DORESLANG_ACTOR_BYTES_TESTING=1
       -I "$root/src/main/cpp"
       "$root/src/main/cpp/oreslang/runtime/NativeOresVM.c"
       "$root/src/main/cpp/oreslang/runtime/NativeActorFuture.c"
       "$root/src/main/cpp/oreslang/runtime/NativeActorScheduler.c"
       "$root/src/main/cpp/oreslang/runtime/NativeActorBytesInbox.c"
       "$root/tests/native_vm_owned_bytes_lifecycle_test.c" )
for cc in gcc clang; do
  for opt in O0 O2; do
    "$cc" "-$opt" "${args[@]}" -o "$tmp/vm-$cc-$opt"
    for run in {1..8}; do
      "$tmp/vm-$cc-$opt" | grep '^NATIVE_VM_OWNED_BYTES_LIFECYCLE_HOST_ONLY_PASS'
    done
    echo "NATIVE_VM_OWNED_BYTES_${cc}_$opt=HOST_ONLY_PASS runs=8"
  done
done
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${args[@]}" -o "$tmp/vm-asan-ubsan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$tmp/vm-asan-ubsan" | grep '^NATIVE_VM_OWNED_BYTES_LIFECYCLE_HOST_ONLY_PASS'
echo "NATIVE_VM_OWNED_BYTES_ASAN_UBSAN=HOST_ONLY_PASS"
echo "ATTO_BYTES_COMPILER=NOT_EXECUTED"
echo "KAFKA_ATTO_NATIVE=NOT_EXECUTED"
