#!/usr/bin/env bash
# Real .atto -> LLVM IR -> Clang native O0/O2 guest execution.
set -euo pipefail
[[ $# -eq 3 ]] || { echo "usage: $0 LLVM_COMPILER CLANG FIXTURES" >&2; exit 64; }
compiler="$1"; clang="$2"; fixtures="$3"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
fail() { echo "DECLARATIVE_EXPORT_NATIVE_FAIL: $*" >&2; exit 1; }
for input in top bottom; do
  "$compiler" --target=host "$fixtures/$input.atto" > "$tmp/$input.ll" 2> "$tmp/$input.err" ||
    fail "$input was rejected: $(cat "$tmp/$input.err")"
  [[ -s "$tmp/$input.ll" ]] || fail "$input emitted no IR"
  grep -Fq '@main(' "$tmp/$input.ll" || fail "$input IR missing main"
  for opt in 0 2; do
    "$clang" "-O$opt" -x ir "$tmp/$input.ll" -o "$tmp/$input-$opt" ||
      fail "$input clang -O$opt compilation"
    exit_code=0
    "$tmp/$input-$opt" || exit_code=$?
    [[ "$exit_code" == 42 ]] || fail "$input -O$opt exit=$exit_code expected 42"
    echo "EXPORT_GUEST_EXECUTED fixture=$input opt=$opt exit=42"
  done
done
for input in wrong-kind unknown duplicate invalid-pkg nested-module-reject; do
  if "$compiler" --target=host "$fixtures/$input.atto" > "$tmp/$input.ll" 2> "$tmp/$input.err"; then
    fail "invalid $input was accepted"
  fi
  [[ ! -s "$tmp/$input.ll" ]] || fail "$input emitted executable IR despite rejection"
  [[ -s "$tmp/$input.err" ]] || fail "$input missing compiler diagnostic"
  echo "EXPORT_REJECTED_NO_IR fixture=$input"
done
