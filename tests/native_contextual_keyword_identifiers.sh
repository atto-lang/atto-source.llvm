#!/usr/bin/env bash
# Java lexer parity (KeywordTaxonomyLexerExecutionTest, spec ch.23): the
# contextual words borrow, critical, enum, gen, pure, requires, serial,
# structural, suspend and using are ordinary identifiers, while reserved
# words (suspends, generator, guard) stay rejected as binding names. The
# legacy `fnc gen* name` compatibility spelling must stay fail-closed even
# though `fnc gen(...)` is now an ordinary function name.
# run-*.atto must compile, link and exit with the status in line 1
# (`// exit: N`, default 42) at O0 and O2. reject-*.atto must fail without
# IR; line 1 is `// expect: <diagnostic>`.
set -euo pipefail
export LC_ALL=C
ulimit -c 0 || true
if [[ $# -ne 2 ]]; then
  echo "usage: $0 <oreslang-llvmc> <clang>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
fixtures="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fixtures/contextual-keyword-identifiers-v1"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/atto-contextual-kw.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
for executable in "$compiler" "$clang"; do
  [[ -x "$executable" ]] || { echo "not executable: $executable" >&2; exit 2; }
done
ran=0
rejected=0
for source in "$fixtures"/run-*.atto; do
  name="$(basename "$source" .atto)"
  expected_status="$(head -n 1 "$source" | sed -n 's|^// exit: ||p')"
  expected_status="${expected_status:-42}"
  ir="$tmp/$name.ll"
  if ! "$compiler" --target=host "$source" >"$ir" 2>"$tmp/$name.stderr"; then
    echo "compiler rejected contextual-keyword fixture $name:" >&2
    cat "$tmp/$name.stderr" >&2
    exit 1
  fi
  test -s "$ir" || { echo "empty IR for $name" >&2; exit 1; }
  for opt in 0 2; do
    binary="$tmp/$name-O$opt"
    "$clang" "-O$opt" -w -x ir "$ir" -o "$binary"
    status=0
    "$binary" >"$tmp/$name-O$opt.stdout" 2>&1 || status=$?
    if [[ "$status" -ne "$expected_status" ]]; then
      echo "$name returned $status at O$opt, expected $expected_status" >&2
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
    echo "compiler accepted reserved-word fixture $name" >&2
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
test "$ran" -eq 5
test "$rejected" -eq 4
echo "contextual keyword identifiers: $ran native O0/O2 guests + $rejected compile negatives passed"
