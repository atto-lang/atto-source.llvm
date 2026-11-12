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
if "$compiler" "$negative" > "$tmp/actor.ll" 2> "$tmp/actor.stderr"; then
  echo "LLVM_KAFKA_ACTOR_UNEXPECTEDLY_COMPILED: must add runtime gate" >&2
  exit 1
fi
test ! -s "$tmp/actor.ll" && test -s "$tmp/actor.stderr"
echo 'ATTO_KAFKA_LLVM_ACTOR=UNSUPPORTED_FAIL_CLOSED'
echo 'ATTO_KAFKA_LLVM_BROKER=NOT_EXECUTED'
