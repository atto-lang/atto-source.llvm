#!/bin/sh
# Real native compiler -> LLVM IR -> Clang -> OS program exit-status oracle.
# Host-only preflight / fake compiler is never credited as backend execution.
set -eu
umask 077
compiler="${1:?require exact built oreslang-llvmc path}"
clang="${2:?require host clang path}"
test -x "$compiler" || { echo "no native Oreslang compiler" >&2; exit 2; }
test -x "$clang" || { echo "no clang" >&2; exit 2; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
passed=0
run_case() {
  source="$1"
  expected="$2"
  printf 'pub fnc main():int{return %s;}\n' "$source" >"$tmp/main.ores"
  "$compiler" "$tmp/main.ores" >"$tmp/guest.ll" 2>"$tmp/compiler.err" || {
    cat "$tmp/compiler.err" >&2
    echo "native source-to-LLVM failed: $source" >&2
    exit 1
  }
  test -s "$tmp/guest.ll" || { echo "empty LLVM IR" >&2; exit 1; }
  "$clang" -x ir "$tmp/guest.ll" -o "$tmp/guest" 2>"$tmp/clang.err" || {
    cat "$tmp/clang.err" >&2
    exit 1
  }
  set +e
  "$tmp/guest" >"$tmp/guest.stdout" 2>"$tmp/guest.stderr"
  status=$?
  set -e
  test "$status" -eq "$expected" || {
    echo "native scalar guest status mismatch for $source: $status != $expected" >&2
    exit 1
  }
  test ! -s "$tmp/guest.stdout" && test ! -s "$tmp/guest.stderr" || {
    echo "unexpected stdout/stderr from scalar native guest" >&2
    exit 1
  }
  passed=$((passed+1))
}
run_case '0>>8' 0
run_case '255>>8' 0
run_case '256>>8' 1
run_case '65535>>8' 255
run_case '255+1>>8' 1
run_case '65536>>8>>8' 1
run_case '(1024*2)>>8' 8
run_case '0>>(8)' 0
reject_case() {
  source="$1"
  printf 'pub fnc main():int{return %s;}\n' "$source" >"$tmp/main.ores"
  if "$compiler" "$tmp/main.ores" >"$tmp/guest.ll" 2>"$tmp/compiler.err"; then
    echo "incorrectly admitted unsupported shift: $source" >&2
    exit 1
  fi
  test ! -s "$tmp/guest.ll" || {
    echo "invalid shift emitted partial LLVM IR" >&2
    exit 1
  }
  test -s "$tmp/compiler.err" || { echo "missing rejection diagnostic" >&2; exit 1; }
  passed=$((passed+1))
}
reject_case '256>>7'
reject_case '256>>8+1'
reject_case '256>>>8'
reject_case '256<<8'
reject_case '256/256'
echo "LLVM_ODB1_SCALAR_SHR8_NATIVE_PASS cases=$passed; NOT FULL DB CODEC OR DURABILITY"
