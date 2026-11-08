#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: native_logical_word_operators.sh <oreslang-llvmc> <clang>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
fixtures="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fixtures/logical-word-operators-v1"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/atto-logical-words.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

for command in "$compiler" "$clang"; do
  if [[ ! -x "$command" ]]; then
    echo "required executable not found: $command" >&2
    exit 2
  fi
done

compile_and_run() {
  local fixture="$1"
  local expected="$2"
  local source="$fixtures/$fixture"
  local ir="$tmp/$fixture.ll"

  if [[ ! -f "$source" ]]; then
    echo "missing logical operator fixture: $source" >&2
    exit 2
  fi
  if ! "$compiler" "$source" >"$ir"; then
    echo "AttoLang compiler rejected positive fixture: $fixture" >&2
    exit 1
  fi
  if [[ ! -s "$ir" ]]; then
    echo "compiler emitted no LLVM IR for: $fixture" >&2
    exit 1
  fi

  for optimization in 0 2; do
    local binary="$tmp/${fixture%.atto}-O$optimization"
    "$clang" "-O$optimization" -x ir "$ir" -o "$binary"
    local status=0
    "$binary" || status=$?
    if [[ "$status" -ne "$expected" ]]; then
      echo "$fixture at -O$optimization returned $status; expected $expected" >&2
      exit 1
    fi
  done
}

reject_reserved_operator_name() {
  local fixture="$1"
  local source="$fixtures/$fixture"
  local ir="$tmp/$fixture.ll"
  local diagnostic="$tmp/$fixture.stderr"
  if "$compiler" "$source" >"$ir" 2>"$diagnostic"; then
    echo "compiler unexpectedly accepted a reserved logical keyword as a function name" >&2
    exit 1
  fi
  if [[ -s "$ir" ]]; then
    echo "reserved keyword declaration emitted executable LLVM IR" >&2
    exit 1
  fi
  if ! grep -Fq "expected non-keyword identifier" "$diagnostic"; then
    echo "reserved keyword was rejected without the expected identifier diagnostic" >&2
    cat "$diagnostic" >&2
    exit 1
  fi
}

reject_removed_caret_operator() {
  local fixture="$1"
  local source="$fixtures/$fixture"
  local ir="$tmp/$fixture.ll"
  local diagnostic="$tmp/$fixture.stderr"
  if "$compiler" "$source" >"$ir" 2>"$diagnostic"; then
    echo "compiler unexpectedly accepted removed caret operator: $fixture" >&2
    exit 1
  fi
  if [[ -s "$ir" ]]; then
    echo "removed caret operator emitted executable LLVM IR: $fixture" >&2
    exit 1
  fi
  if ! grep -Fq "removed scalar operator" "$diagnostic"; then
    echo "missing removed-caret diagnostic for $fixture" >&2
    cat "$diagnostic" >&2
    exit 1
  fi
}

reject_non_boolean() {
  local fixture="$1"
  local source="$fixtures/$fixture"
  local ir="$tmp/$fixture.ll"
  local diagnostic="$tmp/$fixture.stderr"
  if "$compiler" "$source" >"$ir" 2>"$diagnostic"; then
    echo "compiler unexpectedly accepted non-boolean logical fixture: $fixture" >&2
    exit 1
  fi
  if [[ -s "$ir" ]]; then
    echo "rejected fixture emitted executable LLVM IR: $fixture" >&2
    exit 1
  fi
  if ! grep -Fq "logical operator requires bool" "$diagnostic"; then
    echo "missing targeted bool-only diagnostic for $fixture" >&2
    cat "$diagnostic" >&2
    exit 1
  fi
}

compile_and_run scalar-parity.atto 42
compile_and_run keyword-boundaries.atto 42
compile_and_run short-circuit-and.atto 1
compile_and_run short-circuit-or.atto 1
compile_and_run xor-eager.atto 42
compile_and_run xor-left-to-right.atto 42

reject_non_boolean reject-int-and.atto
reject_non_boolean reject-int-or.atto
reject_non_boolean reject-int-xor.atto
reject_removed_caret_operator reject-caret-xor.atto
reject_removed_caret_operator reject-double-caret-xor.atto
reject_reserved_operator_name reject-and-as-name.atto
reject_reserved_operator_name reject-or-as-name.atto
reject_reserved_operator_name reject-word-as-name.atto

echo "PASS: AttoLang logical word operators - truth/precedence, -O0/-O2, short-circuit, eager left-to-right XOR, and bool-only rejection"
