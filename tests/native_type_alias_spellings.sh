#!/usr/bin/env bash
# Java TransparentTypeAliasTest / BooleanTypeAliasTest parity for scalar aliases:
#   type Name = T;   pub type Name = T;   define type alias Name as T;
# are the same transparent alias as `define type Name = T;`, and
# `val Name x = ...` is the same scalar local as `val x: Name = ...`.
# run-*.atto must compile, link and exit 42 at O0 and O2.
# reject-*.atto must fail without IR; line 1 is `// expect: <diagnostic>`.
set -euo pipefail
export LC_ALL=C
ulimit -c 0 || true
if [[ $# -ne 2 ]]; then
  echo "usage: $0 <oreslang-llvmc> <clang>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
fixtures="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fixtures/type-alias-spellings-v1"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/atto-type-alias.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
for executable in "$compiler" "$clang"; do
  [[ -x "$executable" ]] || { echo "not executable: $executable" >&2; exit 2; }
done
ran=0
rejected=0
for source in "$fixtures"/run-*.atto; do
  name="$(basename "$source" .atto)"
  ir="$tmp/$name.ll"
  if ! "$compiler" --target=host "$source" >"$ir" 2>"$tmp/$name.stderr"; then
    echo "compiler rejected positive alias fixture $name:" >&2
    cat "$tmp/$name.stderr" >&2
    exit 1
  fi
  test -s "$ir" || { echo "empty IR for $name" >&2; exit 1; }
  for opt in 0 2; do
    binary="$tmp/$name-O$opt"
    "$clang" "-O$opt" -w -x ir "$ir" -o "$binary"
    status=0
    "$binary" >"$tmp/$name-O$opt.stdout" 2>&1 || status=$?
    if [[ "$status" -ne 42 ]]; then
      echo "$name returned $status at O$opt, expected 42" >&2
      exit 1
    fi
  done
  ran=$((ran + 1))
done
for source in "$fixtures"/reject-*.atto; do
  name="$(basename "$source" .atto)"
  expected="$(head -n 1 "$source" | sed -n 's|^// expect: ||p')"
  [[ -n "$expected" ]] || { echo "$name lacks // expect: line" >&2; exit 2; }
  out="$tmp/$name.ll"
  status=0
  "$compiler" --target=host "$source" >"$out" 2>"$tmp/$name.stderr" || status=$?
  if [[ "$status" -eq 0 ]]; then
    echo "compiler accepted invalid alias fixture $name" >&2
    exit 1
  fi
  if [[ -s "$out" ]]; then
    echo "compiler emitted IR after rejecting $name" >&2
    exit 1
  fi
  if ! grep -Fq -- "$expected" "$tmp/$name.stderr"; then
    echo "$name: missing diagnostic '$expected'; got:" >&2
    cat "$tmp/$name.stderr" >&2
    exit 1
  fi
  rejected=$((rejected + 1))
done
test "$ran" -eq 9
test "$rejected" -eq 5
echo "type alias spellings: $ran native O0/O2 guests + $rejected compile negatives passed"
