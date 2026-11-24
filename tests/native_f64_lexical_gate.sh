#!/usr/bin/env bash
# Real LLVM source -> IR -> Clang -> executable f64 scalar contract.
# Positive cases MUST execute native guest code; negative cases must emit
# no IR. This script is NOT the seven-file num.atto library, Result, List<f64>, or ORIE solver receipt.
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
native_executions=0
hash_evidence() {
  # macOS has shasum but does not ordinarily ship GNU sha256sum.
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@"
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$@"
  else
    echo "NATIVE_F64_SHA256_TOOL_MISSING" >&2
    exit 69
  fi
}
# A repeated case ID must not be able to satisfy totals by substituting for
# a missing fixture. Baseline macOS Bash 3.2-compatible: no associative arrays.
: > "$scratch/.case_ids"
register_case() {
  local case_id="$1"
  [[ "$case_id" =~ ^[a-z0-9_]+$ ]] ||
    { echo "NATIVE_F64_INVALID_CASE_ID: $case_id" >&2; exit 1; }
  if grep -Fxq -- "$case_id" "$scratch/.case_ids"; then
    echo "NATIVE_F64_DUPLICATE_CASE_ID: $case_id" >&2
    exit 1
  fi
  printf '%s\n' "$case_id" >> "$scratch/.case_ids"
}

positive() {
  local name="$1" source="$2" expected_exit="${3:-42}"
  register_case "$name"
  [[ "${#source}" -le 8192 && "$expected_exit" =~ ^[0-9]+$ ]] ||
    { echo "NATIVE_F64_BAD_POSITIVE_INPUT: $name" >&2; exit 1; }
  (( 10#$expected_exit <= 255 )) ||
    { echo "NATIVE_F64_EXIT_OUT_OF_RANGE: $name" >&2; exit 1; }
  printf '%s\n' "$source" > "$scratch/$name.atto"
  "$compiler" --target=host "$scratch/$name.atto" \
    > "$scratch/$name.ll" 2> "$scratch/$name.compiler.stderr" ||
    { echo "NATIVE_F64_COMPILER_REJECTED: $name" >&2
      cat "$scratch/$name.compiler.stderr" >&2; exit 1; }
  [[ -s "$scratch/$name.ll" && ! -s "$scratch/$name.compiler.stderr" ]] ||
    { echo "NATIVE_F64_COMPILER_IR_OR_DIAGNOSTIC: $name" >&2; exit 1; }
  # Do not treat a source-blind constant integer main() as floating-point
  # parity. Every f64 positive has at least one actual double-typed local.
  # The pure integer control is the sole intentional exception.
  grep -Eq '^define[[:space:]].*i32[[:space:]]+@main\(' "$scratch/$name.ll" ||
    { echo "NATIVE_F64_MISSING_NATIVE_MAIN: $name" >&2; exit 1; }
  if [[ "$name" != integer_control ]]; then
    # Match actual LLVM instructions, not '; fadd double' in a forged comment.
    grep -Eq '^[[:space:]]*(%[A-Za-z0-9_.$-]+[[:space:]]*=[[:space:]]*(alloca[[:space:]]+double|load[[:space:]]+double|f(add|sub|mul|div|neg|cmp)[[:space:]].*double)|store[[:space:]]+double)' "$scratch/$name.ll" ||
      { echo "NATIVE_F64_NO_TYPED_DOUBLE_IR: $name" >&2; exit 1; }
  fi
  # Hash the exact source and compiler-produced IR; do not print source contents.
  local fixture_hashes_before
  fixture_hashes_before="$(hash_evidence "$scratch/$name.atto" "$scratch/$name.ll")"
  printf '%s\n' "$fixture_hashes_before"
  # The same compiler-emitted IR must execute correctly with and without
  # Clang optimization. Both runs are authentic native guest executions,
  # never a host/reference calculation or an inferred pass from one binary.
  local opt rc image
  for opt in 0 2; do
    image="$scratch/$name.O$opt.native"
    "$clang" "-O$opt" -Wno-override-module -x ir "$scratch/$name.ll" -o "$image" \
      > "$scratch/$name.O$opt.link.stdout" 2> "$scratch/$name.O$opt.link.stderr" ||
      { echo "NATIVE_F64_LINK_FAILED: $name O$opt" >&2
        cat "$scratch/$name.O$opt.link.stderr" >&2; exit 1; }
    [[ -x "$image" && ! -s "$scratch/$name.O$opt.link.stderr" &&
       ! -s "$scratch/$name.O$opt.link.stdout" ]] ||
      { echo "NATIVE_F64_LINK_DIAGNOSTIC: $name O$opt" >&2; exit 1; }
    # The actual Clang linker must have produced a native image, not a
    # shell/Node shim that merely returns the expected case exit code.
    # Header admission is not independent toolchain provenance/signing.
    local native_magic
    native_magic="$(od -An -tx1 -N4 "$image" | tr -d '[:space:]')"
    case "$native_magic" in
      7f454c46|feedface|feedfacf|cefaedfe|cffaedfe|cafebabe|bebafeca|4d5a*)
        ;;
      *)
        echo "NATIVE_F64_LINK_NOT_NATIVE_IMAGE: $name O$opt" >&2
        exit 1
        ;;
    esac
    local image_hash_before
    image_hash_before="$(hash_evidence "$image")"
    printf '%s\n' "$image_hash_before"
    rc=0
    "$image" > "$scratch/$name.O$opt.stdout" \
      2> "$scratch/$name.O$opt.stderr" || rc=$?
    [[ "$rc" -eq "$expected_exit" && ! -s "$scratch/$name.O$opt.stdout" &&
       ! -s "$scratch/$name.O$opt.stderr" ]] ||
      { echo "NATIVE_F64_EXIT_MISMATCH: $name O$opt rc=$rc expected=$expected_exit" >&2; exit 1; }
    [[ "$(hash_evidence "$image")" == "$image_hash_before" ]] ||
      { echo "NATIVE_F64_EXECUTABLE_MUTATED: $name O$opt" >&2; exit 1; }
    [[ "$(hash_evidence "$scratch/$name.atto" "$scratch/$name.ll")" == "$fixture_hashes_before" ]] ||
      { echo "NATIVE_F64_SOURCE_OR_IR_MUTATED: $name O$opt" >&2; exit 1; }
    native_executions=$((native_executions + 1))
    echo "NATIVE_F64_GUEST_EXACT: $name O$opt"
  done
  pass=$((pass + 1))
}

negative() {
  local name="$1" source="$2" expected_diagnostic="$3" target="${4:-host}"
  register_case "$name"
  [[ "${#source}" -le 8192 && "${#expected_diagnostic}" -gt 8 &&
     "${#expected_diagnostic}" -le 300 ]] ||
    { echo "NATIVE_F64_BAD_NEGATIVE_INPUT: $name" >&2; exit 1; }
  printf '%s\n' "$source" > "$scratch/$name.atto"
  local compile_rc=0
  "$compiler" "--target=$target" "$scratch/$name.atto" \
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
positive f64_nested_local_read_exit_37 'pub fnc main(): int {
  val float first = 0.1;
  val float second = first + 0.2;
  val float third = second * 2.0;
  if third > 0.59; then
    if third < 0.61; then return 37; fi
  fi
  return 86;
}' 37
positive f64_repeated_variable_reads_exit_95 'pub fnc main(): int {
  val f64 base = 1.25;
  val f64 twice = base + base;
  val f64 total = twice + base;
  if total == 3.75; then return 95; fi
  return 86;
}' 95

positive f64_false_ordered_exit_86 'pub fnc main(): int {
  val float value = 0.3;
  if value > 0.7; then return 42; fi
  return 86;
}' 86
positive f64_false_equal_exit_17 'pub fnc main(): int {
  val f64 value = 0.3 + 0.7;
  if value == 1.1; then return 42; fi
  return 17;
}' 17
positive f64_nan_is_not_less_exit_29 'pub fnc main(): int {
  val f64 nan = 0.0 / 0.0;
  if nan < 0.0; then return 42; fi
  return 29;
}' 29


# Numerical M1 scalar prerequisites only: these do NOT execute num.atto's
# imported f64 List/Result/test-harness library or sqrt implementation.
positive attolang_float_alias 'pub fnc main(): int {
  val float value = 6.0 * 7.0;
  if value == 42.0; then return 42; fi
  return 86;
}'

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


positive typed_f64_calls 'fnc blend(float left, f64 right): f64 {
  return left * 2.0 + right;
}
pub fnc main(): int {
  val f64 result = blend(right: 7.0, left: 18.0);
  if result == 43.0; then return 43; fi
  return 86;
}' 43
positive f64_branch_return 'fnc magnitude(f64 value): float {
  if value < 0.0; then return -value; fi
  return value;
}
pub fnc main(): int {
  val float result = magnitude(-8.0) + magnitude(3.5);
  if result == 11.5; then return 44; fi
  return 86;
}' 44
cat > "$scratch/f64_kernel.atto" <<'ATTO'
pub fnc squared(f64 value): float {
  return value * value;
}
ATTO
positive imported_f64_call 'import fnc squared from "./f64_kernel.atto";
pub fnc main(): int {
  val f64 result = squared(3.5);
  if result == 12.25; then return 45; fi
  return 86;
}' 45

negative integer_argument_at_f64_call 'fnc helper(float value): int {
  return 42;
}
pub fnc main(): int { return helper(1); }' 'requires f64'
negative float_alias_entry_return 'pub fnc main(): float { return 0.3; }' 'native main requires zero arguments and an int or void result'
negative decimal_suffix_not_ieee_f64 'pub fnc main(): int {
  val float value = 1.25d;
  return 42;
}' 'native exact decimal literal unsupported until typed Decimal lowering'

negative float_alias_int_coercion 'pub fnc main(): int {
  val float value = 42;
  return 42;
}' 'native scalar local initializer requires f64'

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
negative f64_entry_return 'pub fnc main(): f64 { return 0.3; }' 'native main requires zero arguments and an int or void result'
negative f64_borrow_parameter 'fnc f(borrow f64 value): int {
  return 42;
}
pub fnc main(): int { return 42; }' 'native f64 parameter requires a by-value binding'
negative f64_non_host_call 'fnc helper(float value): f64 {
  return value * value;
}
pub fnc main(): int { return 42; }' 'native f64 function ABI requires the host target' wasm32
negative unsupported_branch_f64_local 'pub fnc main(): int {
  if true; then
    val f64 value = 0.3;
    return 42;
  else return 86; fi
}' 'native branch-local f64 storage requires scoped double lowering'

[[ "$pass" -eq 28 && "$reject" -eq 17 && "$native_executions" -eq 56 ]] ||
  { echo "NATIVE_F64_TEST_COUNT_MISMATCH positive=$pass negative=$reject" >&2; exit 1; }
echo "NATIVE_F64_SCALAR_GUESTS_PASS=28; NATIVE_F64_O0_O2_EXECUTIONS=56; NEGATIVE_NO_IR_PASS=17 (M1 scalar prereqs only; NOT full num.atto library execution)"
