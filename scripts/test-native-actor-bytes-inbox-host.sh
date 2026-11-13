#!/usr/bin/env bash
# Trusted native runtime only. NOT Atto Bytes lowering, no Kafka/async IO.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
for tool in git clang gcc; do
  command -v "$tool" >/dev/null || { echo "NOT_EXECUTED: missing $tool" >&2; exit 69; }
done
if [[ -z "${EXPECTED_LLVM_HEAD:-}" || ! "$EXPECTED_LLVM_HEAD" =~ ^[0-9a-f]{40}$ ]] ||
   [[ "$(git -C "$root" rev-parse HEAD)" != "$EXPECTED_LLVM_HEAD" ]] ||
   [[ -n "$(git -C "$root" status --porcelain --untracked-files=no)" ]]; then
  echo "NOT_EXECUTED: missing exact/clean LLVM source head" >&2
  exit 65
fi
for file in src/main/cpp/oreslang/runtime/NativeActorBytesInbox.h \
            src/main/cpp/oreslang/runtime/NativeActorBytesInbox.c \
            tests/native_actor_bytes_inbox_test.c; do
  test -f "$root/$file" && test ! -L "$root/$file"
  expected="$(git -C "$root" rev-parse "HEAD:$file")"
  actual="$(git -C "$root" hash-object "$root/$file")"
  test "$expected" = "$actual"
  echo "SOURCE_BLOB $file=$actual"
done
echo "LLVM_SOURCE_SHA=$EXPECTED_LLVM_HEAD"
echo "LLVM_SOURCE_TREE=$(git -C "$root" rev-parse 'HEAD^{tree}')"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
args=( -std=c11 -Wall -Wextra -Wpedantic -Werror -UNDEBUG
       -DORESLANG_ACTOR_BYTES_TESTING -I "$root/src/main/cpp"
       "$root/src/main/cpp/oreslang/runtime/NativeActorBytesInbox.c"
       "$root/tests/native_actor_bytes_inbox_test.c" )
for cc in gcc clang; do
  for opt in O0 O2; do
    "$cc" "-$opt" "${args[@]}" -o "$tmp/$cc-$opt"
    "$tmp/$cc-$opt"
    echo "NATIVE_BYTES_INBOX_${cc}_$opt=HOST_ONLY_PASS"
  done
done
clang -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  "${args[@]}" -o "$tmp/clang-sanitizers"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$tmp/clang-sanitizers"
echo "NATIVE_BYTES_INBOX_ASAN_UBSAN=HOST_ONLY_PASS"
echo "ATTO_BYTES_SOURCE_COMPILER=NOT_EXECUTED"
echo "KAFKA_NATIVE_BYTES_BROKER=NOT_EXECUTED"
