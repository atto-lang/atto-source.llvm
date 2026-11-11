#!/usr/bin/env bash
# Genuine LLVM guest compiler -> IR -> clang native executable gate.
# Usage: bash tests/atto_atomic_neq_native.sh /absolute/path/oreslang-llvmc clang
set -Eeuo pipefail
if (( $# != 2 )); then echo "usage: $0 COMPILER CLANG" >&2; exit 64; fi
compiler="$1"
clang="$2"
[[ -x "$compiler" ]] || { echo "Real compiler missing" >&2; exit 2; }
command -v "$clang" >/dev/null || { echo "Clang missing" >&2; exit 2; }
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
base="tests/atto-atomic-neq"
for file in positive.atto reject-split.atto reject-suffix.atto reject-mixed-bool.atto; do
  path="$base/$file"
  test -f "$path" || { echo "Missing fixture $file" >&2; exit 1; }
  test "$(git hash-object --no-filters "$path")" = "$(git rev-parse "HEAD:$path")" || {
    echo "Fixture not byte-identical to exact branch HEAD: $path" >&2; exit 1;
  }
done
work="$(mktemp -d "${TMPDIR:-/tmp}/atto-llvm-neq.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$base/positive.atto" > "$work/positive.ll" 2> "$work/positive.err" || {
  cat "$work/positive.err" >&2; echo "Real compiler rejected positive" >&2; exit 1;
}
test -s "$work/positive.ll" || { echo "Compiler emitted empty LLVM IR" >&2; exit 1; }
grep -Eq '^define i32 @main\(' "$work/positive.ll" || {
  echo "Missing compiled native main" >&2; exit 1;
}
for opt in 0 2; do
  "$clang" "-O$opt" -x ir "$work/positive.ll" -o "$work/guest-O$opt"
  test -x "$work/guest-O$opt" || { echo "Missing native guest at O$opt" >&2; exit 1; }
  set +e
  "$work/guest-O$opt" > "$work/output-O$opt"
  status=$?
  set -e
  if [[ "$status" -ne 42 ]]; then
    printf 'Atto inequality native exit mismatch O%s: got %s, wanted 42\n' "$opt" "$status" >&2
    exit 1
  fi
  echo "ATTO_LLVM_EQ_NEQ_ATOMIC_NATIVE_GUEST_EXECUTED optimizer=O$opt exit=42"
done
for bad in reject-split reject-suffix reject-mixed-bool; do
  if "$compiler" --target=host "$base/$bad.atto" > "$work/$bad.ll" 2> "$work/$bad.err"; then
    echo "Invalid equality input compiled: $bad" >&2; exit 1
  fi
  test ! -s "$work/$bad.ll" || { echo "Rejected source emitted LLVM IR: $bad" >&2; exit 1; }
  test -s "$work/$bad.err" || { echo "No negative diagnostic: $bad" >&2; exit 1; }
done
echo 'ATTO_LLVM_EQ_NEQ_NEGATIVE_CONTROLS_REJECTED_NO_IR'
