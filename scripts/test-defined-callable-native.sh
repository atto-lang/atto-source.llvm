#!/usr/bin/env bash
# Exercise the actual compiler and native machine code, not a parser mock.
set -euo pipefail
compiler="${1:?absolute LLVM compiler path required}"
[[ "$compiler" = /* && -f "$compiler" && -x "$compiler" ]]
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
positive() {
  local name="$1" text="$2"
  printf '%s\n' "$text" >"$work/$name.atto"
  "$compiler" --target=host "$work/$name.atto" >"$work/$name.ll"
  test -s "$work/$name.ll"
  for opt in O0 O2; do
    clang "-$opt" "$work/$name.ll" -o "$work/$name.$opt"
    local status=0
    "$work/$name.$opt" >"$work/$name.stdout" 2>"$work/$name.stderr" || status=$?
    test "$status" = 42
    test ! -s "$work/$name.stdout"
    test ! -s "$work/$name.stderr"
  done
  printf 'PASS defined-callable %s O0/O2\n' "$name"
}
negative() {
  local name="$1" text="$2" diagnostic="$3"
  printf '%s\n' "$text" >"$work/$name.atto"
  if "$compiler" --target=host "$work/$name.atto" >"$work/$name.ll" 2>"$work/$name.err"; then
    echo "Unexpected admission: $name" >&2; exit 1
  fi
  test ! -s "$work/$name.ll"
  grep -F "$diagnostic" "$work/$name.err"
  printf 'PASS defined-callable rejection %s\n' "$name"
}
positive define-pub 'define pub fnc main(): int { return 42; }'
positive pub-define 'pub define fnc main(): int { return 42; }'
positive private-helper 'define fnc answer(): int { return 42; } pub fnc main(): int { return answer(); }'
positive following-function 'fnc answer(): int { return 42; } define pub fnc main(): int { return answer(); }'
negative duplicate-pub 'pub define pub fnc main(): int { return 42; }' 'duplicate pub modifier'
negative public-module 'define pub module M as pub fnc main(): int { return 42; } end' 'native public module wrapper is not supported'
echo DEFINED_CALLABLE_NATIVE_PASS positives=4 optimizations=2 negatives=2
