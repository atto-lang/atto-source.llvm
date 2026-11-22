#!/usr/bin/env bash
# Bound the unsupported-Map/Entry diagnostic and artifact security boundary.
# This is a negative admission gate, NOT guest-runtime conformance.
set -euo pipefail
cd "$(dirname "$0")/.."
compiler=target/release/atto-llvm
test -x "$compiler" || { echo "build atto-llvm before running" >&2; exit 1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for name in dual-entry-add empty-map; do
  outfile="$work/$name.exe"
  if "$compiler" "tests/fixtures/map-entry/$name.atto" -o "$outfile" >"$work/$name.log" 2>&1; then
    echo "FAIL: unsupported $name compiled" >&2
    exit 1
  fi
  grep -Fq 'E_UNSUPPORTED_MAP_ENTRY:' "$work/$name.log" || {
    cat "$work/$name.log" >&2
    echo "FAIL: missing precise Map/Entry diagnostic" >&2
    exit 1
  }
  test ! -e "$outfile" || { echo "FAIL: unsupported Map/Entry published an artifact" >&2; exit 1; }
done

# Closed Record bracket updates are a typed fixed-slot operation, not a
# scalar property write; nominal Struct bracket writes are *never* valid.
for spec in 'record-static-write:E_UNSUPPORTED_RECORD:' \
            'struct-bracket-denied:E_UNSUPPORTED_INDEX_WRITE:'; do
  name="${spec%%:*}"
  diagnostic="${spec#*:}"
  outfile="$work/$name.exe"
  if "$compiler" "tests/fixtures/map-entry/$name.atto" -o "$outfile" >"$work/$name.log" 2>&1; then
    echo "FAIL: typed indexed $name compiled as scalar" >&2
    exit 1
  fi
  grep -Fq "$diagnostic" "$work/$name.log" || {
    cat "$work/$name.log" >&2
    echo "FAIL: missing $diagnostic for $name" >&2
    exit 1
  }
  test ! -e "$outfile" || { echo "FAIL: unlowered $name published an artifact" >&2; exit 1; }
done

# String content must not be mistaken for the Entry operator.
if "$compiler" tests/fixtures/map-entry/string-pseudocode.atto \
    -o "$work/string.exe" >"$work/string.log" 2>&1; then
  echo "FAIL: unsupported string guest compiled" >&2; exit 1
fi
if grep -Eq 'E_UNSUPPORTED_MAP_ENTRY:|E_UNSUPPORTED_RECORD:|E_UNSUPPORTED_INDEX_WRITE:' "$work/string.log"; then
  echo "FAIL: quoted text tripped Entry gate" >&2; exit 1
fi
test ! -e "$work/string.exe"

"$compiler" tests/fixtures/map-entry/comment-pseudocode.atto -o "$work/comment.exe"
set +e
"$work/comment.exe"
result=$?
set -e
test "$result" -eq 42 || { echo "FAIL: scalar comment-control exit=$result" >&2; exit 1; }
echo "ATTO_LLVM_MAP_ENTRY_ADMISSION_PASS unsupported=4 comment_guest_exit=42"
