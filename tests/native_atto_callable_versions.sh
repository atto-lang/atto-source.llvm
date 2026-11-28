#!/usr/bin/env bash
set -euo pipefail

compiler="${1:?native compiler path}"
clang="${2:?clang path}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

compile_and_run() {
  local name="$1" version="$2" body="$3"
  printf '%s\n' "$body" > "$work/$name.atto"
  "$compiler" "--language-version=$version" "$work/$name.atto" > "$work/$name.ll" 2>"$work/$name.err"
  test -s "$work/$name.ll" || { echo "$name: no LLVM IR" >&2; exit 1; }
  "$clang" -x ir "$work/$name.ll" -o "$work/$name.exe"
  set +e
  "$work/$name.exe" >/dev/null
  local code=$?
  set -e
  test "$code" -eq 42 || { echo "$name: wrong guest exit $code" >&2; exit 1; }
}

reject_without_ir() {
  local name="$1" version="$2" body="$3" expected="$4"
  printf '%s\n' "$body" > "$work/$name.atto"
  if "$compiler" "--language-version=$version" "$work/$name.atto" > "$work/$name.ll" 2>"$work/$name.err"; then
    echo "$name: invalid source compiled" >&2; exit 1
  fi
  test ! -s "$work/$name.ll" || { echo "$name: rejected source emitted IR" >&2; exit 1; }
  grep -F "$expected" "$work/$name.err" >/dev/null ||
    { echo "$name: expected $expected; got $(cat "$work/$name.err")" >&2; exit 1; }
}

compile_and_run v1_unmarked 1 'pub fnc main() -> int 1, 42 end'
compile_and_run v1_marked 1 'pub fnc main() -> int :: 1, 42 end'
compile_and_run v2_marked 2 'pub fnc main() -> int :: 1, 42 end'
compile_and_run v2_return 2 'pub fnc main() -> int return 42; end'
compile_and_run v2_caret 2 'pub fnc main() -> int ^ 42; end'
reject_without_ir v2_unmarked 2 'pub fnc main() -> int 1, 42 end' E_LANGUAGE_VERSION
reject_without_ir v1_semicolon 1 'pub fnc main() -> int 1; 42 end' 'comma'
reject_without_ir v2_semicolon_colon 2 'pub fnc main() -> int :: 1; 42 end' 'comma'
reject_without_ir v2_trailing_comma 2 'pub fnc main() -> int :: 1, end' 'comma'
printf '%s\n' 'pub fnc main() -> int :: 42 end' > "$work/invalid_version.atto"
for version in 0 3 -1 junk; do
  if "$compiler" "--language-version=$version" "$work/invalid_version.atto" > "$work/invalid.ll" 2>"$work/invalid.err"; then
    echo "unexpected version $version accepted" >&2; exit 1
  fi
  test ! -s "$work/invalid.ll"
  grep -F E_LANGUAGE_VERSION "$work/invalid.err" >/dev/null
done
if "$compiler" --language-version=1 --language-version=2 "$work/invalid_version.atto" > "$work/dup.ll" 2>"$work/dup.err"; then
  echo "duplicate versions admitted" >&2; exit 1
fi
test ! -s "$work/dup.ll"
grep -F E_LANGUAGE_VERSION "$work/dup.err" >/dev/null

echo 'ATTO_LLVM_CALLABLE_VERSION_NATIVE_PASS|guest=5|negative=9|full_parity=false'
