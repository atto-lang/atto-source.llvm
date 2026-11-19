#!/usr/bin/env bash
# LLVM source->IR->Clang execution smoke for the reviewed portable scalar/v1 corpus.
# Source compiler admission and OS exit code are NOT full guest i64-value parity.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
compiler="${1:-build/oreslang-llvmc}"
[[ -x "$compiler" ]] || { echo 'missing built native LLVM compiler' >&2; exit 2; }
manifest=contracts/parity/portable-scalar-v1-20261101.json
native=tests/portable-scalar-v1-20261101.ores
test "$(git hash-object "$manifest")" = 001e8894cbeb87509878a4de36263a1a327805b1
test "$(git hash-object "$native")" = aea722da9dc85ca03f3646dfaea81f0a821c7360
test "$(grep -c '"id":' "$manifest")" -eq 22
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
accepted=0
executed=0
for record in \
  'zero|0|0' \
  'positive-forty-two|6 * 7|42' \
  'negative-forty-two|-7 * 6|-42' \
  'max-i64|9223372036854775807|9223372036854775807' \
  'min-i64|-9223372036854775808|-9223372036854775808' \
  'max-minus-one|9223372036854775807 - 1|9223372036854775806' \
  'min-plus-one|-9223372036854775808 + 1|-9223372036854775807' \
  'over-binary64-mantissa|9007199254740993 - 9007199254740992|1' \
  'negative-over-binary64|-9007199254740993 - (-9007199254740992)|-1' \
  'precedence|2 + 3 * 4|14' \
  'parentheses|(2 + 3) * 4|20' \
  'subtraction-left-assoc|20 - 5 - 3|12' \
  'grouped-subtraction|20 - (5 - 3)|18' \
  'double-negative|-(-42)|42' \
  'two-negative-factors|(-7) * (-6)|42' \
  'underscore-literal|1_000 + 24|1024'
do
  IFS='|' read -r id expression expected <<< "$record"
  grep -Fq -- "\"id\": \"$id\"" "$manifest"
  grep -Fq -- "\"expression\": \"$expression\"" "$manifest"
  source="$work/$id.ores"; ir="$work/$id.ll"; exe="$work/$id"
  printf 'pub fnc main():int{return %s;}\n' "$expression" > "$source"
  "$compiler" "$source" > "$ir" 2> "$work/$id.stderr" || {
    echo "portable LLVM compiler rejected valid scalar: $id" >&2
    cat "$work/$id.stderr" >&2
    exit 1
  }
  test -s "$ir"
  clang -x ir "$ir" -o "$exe"
  accepted=$((accepted+1))
  if [[ "$expected" =~ ^[0-9]+$ ]] && (( expected <= 255 )); then
    if "$exe" > "$work/$id.stdout" 2> "$work/$id.guest-stderr"; then status=0; else status=$?; fi
    test "$status" -eq "$expected" || {
      echo "LLVM executable status mismatch: $id got $status expected $expected" >&2
      exit 1
    }
    test ! -s "$work/$id.stdout"
    test ! -s "$work/$id.guest-stderr"
    executed=$((executed+1))
  fi
done
rejected=0
for record in \
  'div-int|7 / 2' \
  'forbidden-caret|7 ^ 3' \
  'forbidden-double-caret|7 ^^ 3' \
  'forbidden-remainder|7 % 3' \
  'i64-overflow-literal|9223372036854775808' \
  'negative-i64-underflow-literal|-9223372036854775809'
do
  IFS='|' read -r id expression <<< "$record"
  grep -Fq -- "\"id\": \"$id\"" "$manifest"
  grep -Fq -- "\"expression\": \"$expression\"" "$manifest"
  source="$work/$id.ores"; ir="$work/$id.ll"
  printf 'pub fnc main():int{return %s;}\n' "$expression" > "$source"
  if "$compiler" "$source" > "$ir" 2> "$work/$id.stderr"; then
    echo "LLVM compiler admitted forbidden scalar: $id" >&2
    exit 1
  fi
  test ! -s "$ir" || { echo "LLVM rejected source left partial IR: $id" >&2; exit 1; }
  test -s "$work/$id.stderr"
  rejected=$((rejected+1))
done
test "$accepted" -eq 16
test "$rejected" -eq 6
# Never call a mere compile of i64::MAX full-value execution evidence: the
# existing native main wrapper translates only status codes 0..255.
echo "LLVM_PORTABLE_SCALAR_AUTHORED_22 admitted=$accepted rejected=$rejected actual_os_exit_runs=$executed full_i64_guest_evidence=unverified"
