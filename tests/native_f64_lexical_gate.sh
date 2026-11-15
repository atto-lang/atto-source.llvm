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
# A duplicate ID could overwrite a previous source, IR or executable and
# manufacture an apparent N/N pass. Enforce unique sandbox-safe case IDs.
seen_case_ids=" "
check_case_id() {
  local name="$1"
  [[ "$name" =~ ^[a-z][a-z0-9_]*$ ]] ||
    { echo "NATIVE_F64_UNSAFE_CASE_ID: $name" >&2; exit 1; }
  [[ "$seen_case_ids" != *" $name "* ]] ||
    { echo "NATIVE_F64_DUPLICATE_CASE_ID: $name" >&2; exit 1; }
  seen_case_ids+="$name "
}

positive() {
  local name="$1" source="$2"
  check_case_id "$name"
  printf '%s\n' "$source" > "$scratch/$name.ores"
  "$compiler" --target=host "$scratch/$name.ores" \
    > "$scratch/$name.ll" 2> "$scratch/$name.compiler.stderr" ||
    { echo "NATIVE_F64_COMPILER_REJECTED: $name" >&2
      cat "$scratch/$name.compiler.stderr" >&2; exit 1; }
  [[ -s "$scratch/$name.ll" && ! -s "$scratch/$name.compiler.stderr" ]] ||
    { echo "NATIVE_F64_COMPILER_IR_OR_DIAGNOSTIC: $name" >&2; exit 1; }
  "$clang" -Wno-override-module -x ir "$scratch/$name.ll" -o "$scratch/$name.native" \
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
  check_case_id "$name"
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
positive scoped_f64_then 'pub fnc main(): int {
  if true; then
    val f64 weight = 0.5;
    val f64 prediction = weight * 4.0;
    if prediction == 2.0; then return 42; fi
  fi
  return 86;
}'
positive scoped_f64_else 'pub fnc main(): int {
  if false; then
    return 86;
  else
    val f64 value = 7.0 / 2.0;
    if value == 3.5; then return 42; fi
  fi
  return 86;
}'
positive scoped_f64_mutation 'pub fnc main(): int {
  if true; then
    let f64 weight = 0.5;
    weight = weight + 0.25;
    if weight == 0.75; then return 42; fi
  fi
  return 86;
}'
positive integer_control 'pub fnc main(): int { return 42; }'
# Numerical M1 scalar prerequisites (from #1236): these do NOT execute
# num.atto's imported f64 List/Result/test-harness library.
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
positive f64_abi_gradient 'fnc gradient(f64 weight, f64 x, f64 target): f64 {
  return 2.0 * (weight * x + 1.0 - target) * x;
}
pub fnc main(): int {
  val f64 actual = gradient(5.0, 3.0, 9.0);
  if actual == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_nested_call 'fnc twice(f64 value): f64 { return value * 2.0; }
fnc fourfold(f64 value): f64 { return twice(value) + twice(value); }
pub fnc main(): int {
  if fourfold(10.5) == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_mixed_args 'fnc shifted(f64 weight, int bias): f64 {
  return weight + 0.5;
}
pub fnc main(): int {
  val f64 value = shifted(41.5, 0);
  if value == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_return_local 'fnc pass_through(f64 input): f64 {
  val f64 local = input + 0.0;
  return local;
}
pub fnc main(): int {
  if pass_through(42.0) == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_guarded_return 'fnc choose_positive(f64 candidate): f64 {
  if candidate > 0.0; then return 42.0; fi
  return 86.0;
}
pub fnc main(): int {
  if choose_positive(1.0) == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_branch_return 'fnc branch(f64 seed): f64 {
  if true; then
    val f64 value = seed + 1.0;
    return value;
  fi
  return 86.0;
}
pub fnc main(): int {
  if branch(41.0) == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_int_result 'fnc compare(f64 input): int {
  if input == 42.0; then return 42; fi
  return 86;
}
pub fnc main(): int { return compare(42.0); }'
positive f64_abi_signed_zero 'fnc inverse(f64 value): f64 {
  return 1.0 / value;
}
pub fnc main(): int {
  if inverse(-0.0) < 0.0; then return 42; fi
  return 86;
}'
positive f64_abi_named_reorder 'fnc weighted(f64 x, f64 bias): f64 {
  return x + bias;
}
pub fnc main(): int {
  if weighted(bias: 1.0, x: 41.0) == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_forward_call 'fnc first(f64 x): f64 { return later(x); }
fnc later(f64 x): f64 { return x + 1.0; }
pub fnc main(): int {
  if first(41.0) == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_ternary_return 'fnc choose_value(bool use_first): f64 {
  return use_first ? 42.0 : 86.0;
}
pub fnc main(): int {
  if choose_value(true) == 42.0; then return 42; fi
  return 86;
}'
positive f64_abi_bool_return 'fnc same_value(f64 value): bool {
  return value == 42.0;
}
pub fnc main(): int {
  if same_value(42.0); then return 42; fi
  return 86;
}'
positive oresllm_scalar_gradient 'pub fnc main(): int {
  val f64 weight = 5.0;
  val f64 x = 3.0;
  val f64 bias = 1.0;
  val f64 target = 9.0;
  val f64 error = weight * x + bias - target;
  val f64 gradient = 2.0 * error * x;
  if gradient == 42.0; then return 42; fi
  return 86;
}'
positive oresllm_finite_difference 'pub fnc main(): int {
  val f64 weight = 5.0;
  val f64 x = 3.0;
  val f64 h = 0.5;
  val f64 a = (weight + h) * x + 1.0 - 9.0;
  val f64 b = (weight - h) * x + 1.0 - 9.0;
  val f64 gradient = (a * a - b * b) / (2.0 * h);
  if gradient == 42.0; then return 42; fi
  return 86;
}'
positive oresllm_scoped_gradient 'pub fnc main(): int {
  if true; then
    let f64 gradient = 2.0 * ((5.0 * 3.0 + 1.0) - 9.0) * 3.0;
    gradient = gradient + 0.0;
    if gradient == 42.0; then return 42; fi
  fi
  return 86;
}'

negative f64_abi_main_return 'pub fnc main(): f64 {
  return 42.0;
}' 'native main does not admit f64 process ABI'
negative f64_abi_main_parameter 'pub fnc main(f64 value): int {
  return 42;
}' 'native main does not admit f64 process ABI'
negative f64_abi_borrow_parameter 'fnc unproven(borrow f64 value): f64 {
  return value;
}
pub fnc main(): int { return 42; }' 'native f64 parameter must be passed by value'
positive f64_literal_call_argument 'fnc pass_through(f64 value): f64 {
  return value;
}
pub fnc main(): int {
  val f64 result = pass_through(42.0);
  if result == 42.0; then return 42; fi
  return 86;
}'
positive f64_literal_function_return 'fnc widened(f64 value): f64 {
  return 42.0;
}
pub fnc main(): int {
  if widened(1.0) == 42.0; then return 42; fi
  return 86;
}'
negative f64_abi_no_implicit_downcast 'fnc wrong(int value): int { return value; }
pub fnc main(): int {
  val int result = wrong(42.0);
  return result;
}' "argument 'value' requires int, got f64"
# Canonical language does not implicitly widen int to float.
positive f64_literal_binding 'pub fnc main(): int {
  val f64 value = 1.0;
  if value == 1.0; then return 42; fi
  return 86;
}'
positive same_type_f64_add 'pub fnc main(): int {
  val f64 value = 0.5 + 1.0;
  if value == 1.5; then return 42; fi
  return 86;
}'
positive same_type_f64_compare 'pub fnc main(): int {
  val f64 value = 0.5;
  if value < 1.0; then return 42; fi
  return 86;
}'
negative f64_to_integer_return 'pub fnc main(): int {
  return 1.5;
}' 'native function return requires int, got f64'
negative malformed_exponent 'pub fnc main(): int {
  val f64 value = 1e+;
  return 42;
}' "expected ';'"
# Java rounds an out-of-range literal to infinity.
positive literal_overflow_is_infinity 'pub fnc main(): int {
  val f64 value = 1e9999;
  if value > 1.0e308; then return 42; fi
  return 86;
}'
negative implicit_bool_to_f64 'pub fnc main(): int {
  val f64 value = true;
  return 42;
}' 'native scalar local initializer requires f64, got bool'
negative int_division 'pub fnc main(): int {
  val int value = 7 / 2;
  return value;
}' 'native scalar local initializer requires int, got f64'
positive same_type_f64_division 'pub fnc main(): int {
  val f64 value = 7.0 / 2.0;
  if value == 3.5; then return 42; fi
  return 86;
}'
negative f64_abi_bool_to_float_return 'fnc wrong_result(f64 value): f64 {
  return true;
}
pub fnc main(): int { return 42; }' 'native function return requires f64, got bool'
negative f64_abi_bool_argument 'fnc only_f64(f64 value): int {
  return 42;
}
pub fnc main(): int { return only_f64(true); }' "argument 'value' requires f64, got bool"
negative f64_abi_named_bool_argument 'fnc offset(f64 x, f64 bias): f64 {
  return x + bias;
}
pub fnc main(): int {
  val f64 y = offset(bias: true, x: 42.0);
  return 42;
}' "argument 'bias' requires f64, got bool"
positive f64_literal_guarded_return 'fnc guarded(f64 x): f64 {
  if x > 0.0; then return 42.0; fi
  return 42.0;
}
pub fnc main(): int {
  if guarded(1.0) == 42.0; then return 42; fi
  return 86;
}'
positive f64_literal_scoped_initializer 'pub fnc main(): int {
  if true; then
    val f64 value = 1.0;
    return 42;
  fi
  return 86;
}'
positive f64_literal_scoped_assignment 'pub fnc main(): int {
  if true; then
    let f64 value = 1.5;
    value = 2.0;
    if value == 2.0; then return 42; fi
  fi
  return 86;
}'

# Removed source operators must fail lexically, before producing LLVM IR.
negative removed_percent 'pub fnc main(): int { return 5 % 3; }' 'removed AttoLang source operator'
negative removed_single_caret 'pub fnc main(): int { return 5 ^ 3; }' 'removed AttoLang source operator'
# Source must fail before IR whenever a numeric conversion is implicit.
negative implicit_f64_binding 'pub fnc main(): int {
  val f64 value = 1;
  return 42;
}' 'requires f64, got int'
negative implicit_f64_argument 'fnc consume(f64 input): f64 { return input; }
pub fnc main(): int { val f64 y = consume(1); return 42; }' 'requires f64, got int'
negative implicit_f64_return 'fnc produce(): f64 { return 1; }
pub fnc main(): int { return 42; }' 'requires f64, got int'
negative implicit_f64_mixed_add 'pub fnc main(): int {
  val f64 value = 1.0 + 2;
  return 42;
}' 'requires explicit numeric conversion'
negative implicit_f64_mixed_equality 'pub fnc main(): int {
  val bool value = 1 == 1.0;
  return 42;
}' 'requires explicit numeric conversion'
negative implicit_f64_mixed_ternary 'pub fnc main(): int {
  val f64 value = true ? 1 : 2.0;
  return 42;
}' 'requires explicit numeric conversion'
negative implicit_f64_assignment 'pub fnc main(): int {
  let f64 value = 1.0;
  value = 2;
  return 42;
}' 'requires f64, got int'
# Cross-feature merge with ODB1 shifts: float operands/distances never
# become integer bit patterns through the scalar f64 lowering.
negative f64_shift_left_operand 'pub fnc main(): int {
  val int value = 1.0 << 2;
  return value;
}' 'native shift operands require int'
negative f64_shift_right_distance 'pub fnc main(): int {
  val int value = 4 >> 1.0;
  return value;
}' 'native shift operands require int'
negative f64_shift_unsigned 'pub fnc main(): int {
  val int value = 8.0 >>> 1;
  return value;
}' 'native shift operands require int'
[[ "$pass" -eq 47 && "$reject" -eq 23 ]] ||
  { echo "NATIVE_F64_TEST_COUNT_MISMATCH positive=$pass negative=$reject" >&2; exit 1; }
echo "NATIVE_F64_SCALAR_GUESTS_PASS=47; NEGATIVE_NO_IR_PASS=23"
