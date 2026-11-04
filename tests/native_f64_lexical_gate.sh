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
  local name="$1" source="$2" expected_diagnostic="$3"
  printf '%s\n' "$source" > "$scratch/$name.ores"
  local compile_rc=0
  "$compiler" --target=host "$scratch/$name.ores" \
      > "$scratch/$name.ll" 2> "$scratch/$name.stderr" || compile_rc=$?
  # The actual compiler's caught source-error contract is exactly exit 1
  # and a structured oreslang-llvmc: diagnostic. Signals, crashes, timeouts,
  # missing binaries and runner failures are NOT successful rejection tests.
  [[ "$compile_rc" -eq 1 ]] ||
    { echo "NATIVE_F64_REJECTION_NOT_SOURCE_ERROR: $name rc=$compile_rc" >&2; exit 1; }
  [[ ! -s "$scratch/$name.ll" ]] &&
    grep -q '^oreslang-llvmc: ' "$scratch/$name.stderr" &&
    grep -Fq -- "$expected_diagnostic" "$scratch/$name.stderr" ||
    { echo "NATIVE_F64_PARTIAL_IR_OR_WRONG_DIAGNOSTIC: $name expected=$expected_diagnostic" >&2
      cat "$scratch/$name.stderr" >&2; exit 1; }
  reject=$((reject + 1))
  echo "NATIVE_F64_REJECTED_NO_IR: $name"
}

positive scalar_local 'pub fnc main(): int {
  val f64 value = 0.3;
  if value > 0.0; then return 42; fi
  return 86;
}'
positive scientific 'pub fnc main(): int {
  val f64 value = 1E-2;
  if value < 0.02; then return 42; fi
  return 86;
}'
positive arithmetic 'pub fnc main(): int {
  val f64 value = (2.5 + 1.5) * 3.0 - 1.0;
  if value == 11.0; then return 42; fi
  return 86;
}'
positive division 'pub fnc main(): int {
  val f64 value = 7.0 / 2.0;
  if value == 3.5; then return 42; fi
  return 86;
}'
positive simplex_rounding 'pub fnc main(): int {
  val f64 value = 0.3 * (0.7 / 0.3);
  if value > 0.7; then return 42; fi
  return 86;
}'
positive signed_zero 'pub fnc main(): int {
  val f64 value = -0.0;
  if value == 0.0; then return 42; fi
  return 86;
}'
positive rounding 'pub fnc main(): int {
  val f64 value = 0.1 + 0.2;
  if value > 0.3; then return 42; fi
  return 86;
}'
positive nan_unordered_not_equal 'pub fnc main(): int {
  val f64 nan = 0.0 / 0.0;
  if nan != nan; then return 42; fi
  return 86;
}'
positive nan_ordered_false 'pub fnc main(): int {
  val f64 nan = 0.0 / 0.0;
  if nan < 1.0; then return 86; fi
  return 42;
}'
positive signed_zero_inverse 'pub fnc main(): int {
  val f64 inverse = 1.0 / (-0.0);
  if inverse < 0.0; then return 42; fi
  return 86;
}'
positive positive_infinity 'pub fnc main(): int {
  val f64 infinity = 1.0 / 0.0;
  if infinity > 1.0; then return 42; fi
  return 86;
}'
positive f64_ternary_true 'pub fnc main(): int {
  val f64 result = true ? 0.3 : 0.7;
  if result == 0.3; then return 42; fi
  return 86;
}'
positive f64_ternary_false 'pub fnc main(): int {
  val f64 result = false ? 0.3 : 0.7;
  if result == 0.7; then return 42; fi
  return 86;
}'
positive integer_control 'pub fnc main(): int { return 42; }'

# Numerical M1 scalar prerequisites only: these do NOT execute num.atto's
# imported f64 List/Result/test-harness library or sqrt implementation.
positive num_m1_negative_domain 'pub fnc main(): int {
  val f64 negative = 0.0 - 1.0;
  if negative < 0.0; then return 42; fi
  return 86;
}'
positive num_m1_tiny_magnitude 'pub fnc main(): int {
  val f64 tiny = 1.0e-100;
  if tiny > 0.0; then
    if tiny < 1.0e-99; then return 42; fi
  fi
  return 86;
}'
positive num_m1_huge_magnitude 'pub fnc main(): int {
  val f64 huge = 1.0e300;
  if huge > 1.0e299; then
    if huge < 1.0e301; then return 42; fi
  fi
  return 86;
}'
positive num_m1_overflow_infinity 'pub fnc main(): int {
  val f64 overflowed = 1.0e308 * 1.0e308;
  if overflowed > 1.0e308; then return 42; fi
  return 86;
}'
positive num_m1_scalar_reduction_seed 'pub fnc main(): int {
  val f64 reduced = 1.0 + 2.0 + 3.0 + 4.0;
  if reduced == 10.0; then return 42; fi
  return 86;
}'


negative implicit_int_to_f64 'pub fnc main(): int {
  val f64 value = 1;
  return 42;
}' 'native scalar local initializer requires f64'
negative mixed_add 'pub fnc main(): int {
  val f64 value = 0.5 + 1;
  return 42;
}' 'arithmetic; implicit int/f64 conversion prohibited'
negative mixed_ordered_compare 'pub fnc main(): int {
  val f64 value = 0.5;
  if value < 1; then return 42; fi
  return 86;
}' 'ordered comparison; implicit int/f64 conversion prohibited'
negative f64_to_integer_return 'pub fnc main(): int {
  return 1.5;
}' 'native function return requires int, got f64'
negative malformed_exponent 'pub fnc main(): int {
  val f64 value = 1e+;
  return 42;
}' 'malformed native floating exponent'
negative out_of_range 'pub fnc main(): int {
  val f64 value = 1e9999;
  return 42;
}' 'native f64 literal outside finite binary64 range or malformed'
negative implicit_bool_to_f64 'pub fnc main(): int {
  val f64 value = true;
  return 42;
}' 'native scalar local initializer requires f64, got bool'
negative int_division 'pub fnc main(): int {
  val int value = 7 / 2;
  return value;
}' 'f64 division (int/int unsupported)'
negative mixed_division 'pub fnc main(): int {
  val f64 value = 7.0 / 2;
  return 42;
}' 'f64 division (no implicit int conversion)'
negative unsupported_f64_return_abi 'pub fnc main(): f64 { return 0.3; }' 'native f64 return ABI not yet admitted'
negative unsupported_f64_parameter_abi 'pub fnc f(f64 value): int {
  return 42;
}
pub fnc main(): int { return 42; }' 'native f64 function parameter ABI not yet admitted'
negative unsupported_branch_f64_local 'pub fnc main(): int {
  if true; then
    val f64 value = 0.3;
    return 42;
  else return 86; fi
}' 'native branch-local f64 storage requires scoped double lowering'

[[ "$pass" -eq 19 && "$reject" -eq 12 ]] ||
  { echo "NATIVE_F64_TEST_COUNT_MISMATCH positive=$pass negative=$reject" >&2; exit 1; }
echo "NATIVE_F64_SCALAR_GUESTS_PASS=19; NEGATIVE_NO_IR_PASS=12 (M1 scalar prereqs only, not num.atto library execution)"
