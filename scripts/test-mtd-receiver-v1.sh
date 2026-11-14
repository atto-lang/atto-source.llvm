#!/usr/bin/env bash
# Execute actual .atto through oreslang-llvmc -> LLVM IR -> native executable.
# This is NOT a source-text simulation, and does not claim Java/.NET parity.
set -euo pipefail
if [[ $# -ne 2 ]]; then
  echo "usage: $0 /path/to/oreslang-llvmc /path/to/clang" >&2
  exit 2
fi
compiler="$1"
clang="$2"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixtures="$repo_root/tests/mtd-receiver-v1"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"$compiler" "$fixtures/positive-local-class.atto" > "$tmp/main.ll" 2> "$tmp/compile.err"
if [[ ! -s "$tmp/main.ll" ]]; then
  echo "native receiver fixture generated no LLVM IR" >&2
  exit 1
fi
"$clang" -O1 "$tmp/main.ll" -o "$tmp/main"
set +e
"$tmp/main"
code=$?
set -e
if [[ $code -ne 42 ]]; then
  echo "expected receiver guest exit=42, got=$code" >&2
  exit 1
fi

check_negative() {
  local file="$1" expected="$2" rc=0
  : > "$tmp/rejected.ll"
  : > "$tmp/rejected.err"
  "$compiler" "$fixtures/$file" > "$tmp/rejected.ll" 2> "$tmp/rejected.err" || rc=$?
  if [[ $rc -eq 0 || -s "$tmp/rejected.ll" ]] || ! grep -Fq "$expected" "$tmp/rejected.err"; then
    echo "FAIL $file: expected diagnostic '$expected', no executable IR" >&2
    sed -n '1,30p' "$tmp/rejected.err" >&2
    exit 1
  fi
}

check_negative negative-missing-receiver.atto NATIVE_MTD_LOCAL_CLASS_REQUIRED
check_negative negative-readonly-mutation.atto 'shared class borrow'
check_negative negative-duplicate-self.atto NATIVE_MTD_DUPLICATE_SELF
check_negative negative-duplicate-slot.atto NATIVE_MTD_DUPLICATE_SLOT
echo "PASS: native mtd receiver guest exit=42 and four fail-closed compiler cases"
