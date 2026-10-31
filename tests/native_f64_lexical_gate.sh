#!/usr/bin/env bash
# Real LLVM source -> IR -> Clang -> executable f64 scalar contract.
# Positive cases MUST execute native guest code; negative cases must emit
# no IR. This script is NOT an Array<f64>, Result, or ORIE solver receipt.
set -euo pipefail
umask 077
export LC_ALL=C
[[ "$#" -eq 2 ]] || { echo "usage: native_f64_lexical_gate.sh <llvmc> <clang>" >&2; exit 64; }
compiler="$1"; clang="$2"
[[ -x "$compiler" ]] && command -v "$clang" >/dev/null ||
  { echo "Missing real LLVM compiler or Clang" >&2; exit 69; }
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
pass=0
reject=0

positive() {
  local name="$1" source="$2"
  printf '%s\n' "$source" > "$scratch/$name.ores"
  "$compiler" --target=host "$scratch/$name.ores" \
    > "$scratch/$name.ll" 2> "$scratch/$name.compiler.stderr" ||
    { echo "NATIVE_F64_COMPILER_REJECTED: $name" >&2
      cat "$scratch/$name.compiler.stderr" >&2; exit 1; }
  [[ -s "$scratch/$name.ll" && ! -s "$scratch/$name.compiler.stderr" ]] ||
    { echo "NATIVE_F64_COMPILER_IR_OR_DIAGNOSTIC: $name" >&2; exit 1; }
  "$clang" -x ir "$scratch/$name.ll" -o "$scratch/$name.native" \
    > "$scratch/$name.link.stdout" 2> "$scratch/$name.link.stderr" ||
    { echo "NATIVE_F64_LINK_FAILED: $name" >&2
      cat "$scratch/$name.link.stderr" >&2; exit 1; }
  [[ -x "$scratch/$name.native" && ! -s "$scratch/$name.link.stderr" ]] ||
    { echo "NATIVE_F64_LINK_DIAGNOSTIC: $name" >&2; exit 1; }
  local rc=0
  "$scratch/$name.native" > "$scratch/$name.stdout" \
    2> "$scratch/$name.stderr" || rc=$?
  [[ "$rc" -eq 42 && ! -s "$scratch/$name.stdout" &&
     ! -s "$scratch/$name.stderr" ]] ||
    { echo "NATIVE_F64_EXIT_MISMATCH: $name rc=$rc" >&2; exit 1; }
  pass=$((pass + 1))
  echo "NATIVE_F64_GUEST_EXACT: $name"
}

negative() {
  local name="$1" source="$2"
  printf '%s\n' "$source" > "$scratch/$name.ores"
  if "$compiler" --target=host "$scratch/$name.ores" \
      > "$scratch/$name.ll" 2> "$scratch/$name.stderr"; then
    echo "NATIVE_F64_FALSE_ACCEPT: $name" >&2; exit 1
  fi
  [[ ! -s "$scratch/$name.ll" && -s "$scratch/$name.stderr" ]] ||
    { echo "NATIVE_F64_PARTIAL_IR_OR_MISSING_ERROR: $name" >&2; exit 1; }
  reject=$((reject + 1))
  echo "NATIVE_F64_REJECTED_NO_IR: $name"
}

positive scalar_local 'pub fnc main(): int {
  val f64 value = 0.3;
  if value > 0.0; then return 42; else return 86; fi
}'
positive scientific 'pub fnc main(): int {
  val f64 value = 1E-2;
  if value < 0.02; then return 42; else return 86; fi
}'
positive arithmetic 'pub fnc main(): int {
  val f64 value = (2.5 + 1.5) * 3.0 - 1.0;
  if value == 11.0; then return 42; else return 86; fi
}'
positive division 'pub fnc main(): int {
  val f64 value = 7.0 / 2.0;
  if value == 3.5; then return 42; else return 86; fi
}'
positive simplex_rounding 'pub fnc main(): int {
  val f64 value = 0.3 * (0.7 / 0.3);
  if value > 0.7; then return 42; else return 86; fi
}'
positive signed_zero 'pub fnc main(): int {
  val f64 value = -0.0;
  if value == 0.0; then return 42; else return 86; fi
}'
positive rounding 'pub fnc main(): int {
  val f64 value = 0.1 + 0.2;
  if value > 0.3; then return 42; else return 86; fi
}'
positive nan_unordered_not_equal 'pub fnc main(): int {
  val f64 nan = 0.0 / 0.0;
  if nan != nan; then return 42; else return 86; fi
}'
positive nan_ordered_false 'pub fnc main(): int {
  val f64 nan = 0.0 / 0.0;
  if nan < 1.0; then return 86; else return 42; fi
}'
positive signed_zero_inverse 'pub fnc main(): int {
  val f64 inverse = 1.0 / (-0.0);
  if inverse < 0.0; then return 42; else return 86; fi
}'
positive positive_infinity 'pub fnc main(): int {
  val f64 infinity = 1.0 / 0.0;
  if infinity > 1.0; then return 42; else return 86; fi
}'
positive integer_control 'pub fnc main(): int { return 42; }'

negative implicit_int_to_f64 'pub fnc main(): int {
  val f64 value = 1;
  return 42;
}'
negative mixed_add 'pub fnc main(): int {
  val f64 value = 0.5 + 1;
  return 42;
}'
negative mixed_ordered_compare 'pub fnc main(): int {
  val f64 value = 0.5;
  if value < 1; then return 42; else return 86; fi
}'
negative f64_to_integer_return 'pub fnc main(): int {
  return 1.5;
}'
negative malformed_exponent 'pub fnc main(): int {
  val f64 value = 1e+;
  return 42;
}'
negative out_of_range 'pub fnc main(): int {
  val f64 value = 1e9999;
  return 42;
}'
negative implicit_bool_to_f64 'pub fnc main(): int {
  val f64 value = true;
  return 42;
}'
negative int_division 'pub fnc main(): int {
  val int value = 7 / 2;
  return value;
}'
negative mixed_division 'pub fnc main(): int {
  val f64 value = 7.0 / 2;
  return 42;
}'
negative unsupported_f64_return_abi 'pub fnc main(): f64 { return 0.3; }'
negative unsupported_f64_parameter_abi 'pub fnc f(f64 value): int {
  return 42;
}
pub fnc main(): int { return 42; }'
negative unsupported_branch_f64_local 'pub fnc main(): int {
  if true; then
    val f64 value = 0.3;
    return 42;
  else return 86; fi
}'

[[ "$pass" -eq 12 && "$reject" -eq 12 ]] ||
  { echo "NATIVE_F64_TEST_COUNT_MISMATCH positive=$pass negative=$reject" >&2; exit 1; }
echo "NATIVE_F64_SCALAR_GUESTS_PASS=12; NEGATIVE_NO_IR_PASS=12 (not arrays/solver)"
