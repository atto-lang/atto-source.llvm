#!/usr/bin/env bash
# Real production native compiler -> LLVM IR -> Clang -> executable.
# The six shared fixtures use exactly the Rust/browser frontend test bytes.
set -euo pipefail
[[ $# == 2 ]] || { echo 'usage: native-boolean-alias-guest.sh LLVMC CLANG' >&2; exit 64; }
compiler="$1"; clang="$2"
[[ -x "$compiler" ]]
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
positive() {
  local id="$1" blob="$2" source="$3"
  printf '%s\n' "$source" > "$work/$id.atto"
  test "$(git hash-object "$work/$id.atto")" = "$blob"
  "$compiler" --target=host "$work/$id.atto" > "$work/$id.ll"
  test -s "$work/$id.ll"
  grep -Eq 'define.*@main\(' "$work/$id.ll"
  for opt in 0 2; do
    "$clang" "-O$opt" -x ir "$work/$id.ll" -o "$work/$id-$opt"
    code=0; "$work/$id-$opt" || code=$?
    test "$code" = 42
    echo "PASS native $id O$opt $blob exit=42"
  done
}
negative() {
  local id="$1" blob="$2" source="$3" diagnostic="$4"
  printf '%s\n' "$source" > "$work/$id.atto"
  test "$(git hash-object "$work/$id.atto")" = "$blob"
  if "$compiler" --target=host "$work/$id.atto" > "$work/$id.ll" 2> "$work/$id.err"; then
    echo "FAIL accepted negative $id" >&2; exit 1
  fi
  test ! -s "$work/$id.ll"
  grep -Eiq "$diagnostic" "$work/$id.err"
  echo "PASS reject $id $blob"
}
positive canonical 9376b682711bd81d5a136b055ac3b8a31f61a784 'fnc yes(bool x):bool{return x;} pub fnc main():int{if yes(true) then return 42; fi return 0;}'
positive alias-param 1d63d115f2dbdc155a0bba284a193e9b47464e50 'fnc yes(boolean x):bool{return x;} pub fnc main():int{if yes(true) then return 42; fi return 0;}'
positive alias-return c4c0d2611d1af2d4ed43beed6273d8cdfadc9d58 'fnc yes(bool x):boolean{return x;} pub fnc main():int{if yes(false) then return 0; fi return 42;}'
positive typed-local 61e1beae3726a37e244b8169acc15cc65e2a54d7 'pub fnc main():int{val boolean x=true; if x then return 42; fi return 0;}'
negative bad-value 378dc816a656da784bf217c2645b33627667aabf 'fnc yes(boolean x):bool{return x;} pub fnc main():int{if yes(1) then return 42; fi return 0;}' 'argument|bool|type mismatch'
negative wrong-case c6555201a174f31fa788cce17bad2fd5de7c0e08 'fnc yes(Boolean x):bool{return true;} pub fnc main():int{if yes(true) then return 42; fi return 0;}' 'unknown|class|type|unsupported'
printf '%s\n' 'type boolean = int; fnc yes(boolean x):bool{return true;} pub fnc main():int{return 42;}' > "$work/shadow.atto"
if "$compiler" --target=host "$work/shadow.atto" > "$work/shadow.ll" 2> "$work/shadow.err"; then
  echo 'FAIL silently erased nominal alias' >&2; exit 1
fi
test ! -s "$work/shadow.ll"
grep -Fq 'E_TYPE_ALIAS_SHADOWED' "$work/shadow.err"
echo 'PASS reject ambiguous shadow'
echo 'NATIVE_BOOLEAN_ALIAS_PASS: 8 native guests, 3 compiler negatives'
