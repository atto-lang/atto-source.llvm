#!/usr/bin/env bash
# Run real Atto guest through native LLVM compilation; reject unsupported
# shapes before any LLVM IR. No host-only reference model is counted.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 || (${3:-} != "" && ${3:-} != "--sanitize") ]]; then
  echo "usage: $0 /path/to/oreslang-llvmc /path/to/clang [--sanitize]" >&2
  exit 2
fi
compiler="$1"
clang="$2"
sanitized="no"
if [[ ${3:-} == "--sanitize" ]]; then sanitized="yes"; fi
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixtures="$repo_root/tests/mtd-receiver-v1"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

"$compiler" "$fixtures/positive-local-class.atto" > "$tmp/main.ll" 2> "$tmp/compile.err"
if [[ ! -s "$tmp/main.ll" ]]; then
  echo "native receiver fixture generated no LLVM IR" >&2
  exit 1
fi

# Exercise both optimization and safety configurations. Running only Clang
# once is insufficient proof that receiver borrowing survives optimizations.
for opt in 0 2; do
  args=(-O"$opt")
  if [[ $sanitized == "yes" ]]; then
    args+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  fi
  "$clang" "${args[@]}" "$tmp/main.ll" -o "$tmp/main-$opt"
  set +e
  "$tmp/main-$opt"
  code=$?
  set -e
  if [[ $code -ne 42 ]]; then
    echo "expected native receiver exit=42 at O$opt sanitized=$sanitized, got=$code" >&2
    exit 1
  fi
done

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
check_negative negative-lifecycle-spoof.atto NATIVE_MTD_RESERVED_MEMBER
check_negative negative-immutable-owner-call.atto immut
echo "PASS: native mtd receiver guest exit=42 at O0/O2 and six fail-closed compiler cases; sanitized=$sanitized"
