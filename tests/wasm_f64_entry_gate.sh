#!/usr/bin/env bash
# Canonical source -> actual LLVM compiler -> target-marked IR contract.
# No stub, no mock, no claim of linked browser guest execution.
set -euo pipefail
umask 077
export LC_ALL=C
[[ "$#" -eq 1 && -x "$1" ]] || { echo "REAL_LLVM_COMPILER_REQUIRED" >&2; exit 69; }
compiler="$1"
dir="$(mktemp -d)"
trap 'rm -rf -- "$dir"' EXIT
printf '%s\n' 'pub fnc main(): f64 { return 1.5 + 2.25; }' > "$dir/float.atto"
printf '%s\n' 'pub fnc main(): int { return 42; }' > "$dir/int.atto"
printf '%s\n' 'fnc helper(): f64 { return 3.75; }' > "$dir/no-main.atto"
printf '%s\n' 'pub fnc main(f64 arg): f64 { return arg; }' > "$dir/param.atto"
pass=0
expect_ir() {
  local name="$1"; shift
  local status=0
  "$compiler" "$@" > "$dir/$name.ll" 2> "$dir/$name.stderr" || status=$?
  [[ "$status" -eq 0 && -s "$dir/$name.ll" && ! -s "$dir/$name.stderr" ]] ||
    { echo "WASM_F64_UNEXPECTED_COMPILER_FAILURE:$name:$status" >&2
      cat "$dir/$name.stderr" >&2; exit 1; }
  grep -Eq '^[[:space:]]*target triple = "wasm32-unknown-unknown"' "$dir/$name.ll" ||
    { echo "WASM_F64_MISSING_TARGET_TRIPLE:$name" >&2; exit 1; }
  grep -Eq '^[[:space:]]*define[[:space:]]+double[[:space:]]+@main[[:space:]]*[(][[:space:]]*[)]' "$dir/$name.ll" ||
    { echo "WASM_F64_MISSING_DOUBLE_MAIN:$name" >&2; exit 1; }
  [[ "$(grep -Ec '^[[:space:]]*define[[:space:]]+double[[:space:]]+@main[[:space:]]*[(][[:space:]]*[)]' "$dir/$name.ll")" -eq 1 ]] ||
    { echo "WASM_F64_DUPLICATE_MAIN:$name" >&2; exit 1; }
  pass=$((pass + 1))
}
reject() {
  local name="$1" expected_status="$2" expected_diag="$3"; shift 3
  local status=0
  "$compiler" "$@" > "$dir/$name.ll" 2> "$dir/$name.stderr" || status=$?
  [[ "$status" -eq "$expected_status" && ! -s "$dir/$name.ll" ]] ||
    { echo "WASM_F64_NOT_REJECTED:$name:$status" >&2; exit 1; }
  grep -Fq -- "$expected_diag" "$dir/$name.stderr" ||
    { echo "WASM_F64_BAD_REJECTION:$name" >&2
      cat "$dir/$name.stderr" >&2; exit 1; }
  pass=$((pass + 1))
}
expect_ir wasm_f64 --library-profile=core --target=wasm32 --wasm-core-abi=f64 --tree-shake --tree-shake-root=main "$dir/float.atto"
reject wasm_default_float 1 "native main does not admit f64 process ABI" --library-profile=core --target=wasm32 "$dir/float.atto"
reject host_default_float 1 "native main does not admit f64 process ABI" --target=host "$dir/float.atto"
reject host_explicit_f64 2 "wasm Core f64 entry ABI requires --target=wasm32" --target=host --wasm-core-abi=f64 "$dir/float.atto"
reject cortex_explicit_f64 2 "wasm Core f64 entry ABI requires --target=wasm32" --target=thumbv7m-none-eabi --wasm-core-abi=f64 "$dir/float.atto"
reject wasm_f64_int_entry 1 "wasm Core f64 entry requires exact main(): f64" --target=wasm32 --wasm-core-abi=f64 "$dir/int.atto"
reject wasm_f64_no_entry 1 "wasm Core f64 entry requires exact main(): f64" --target=wasm32 --wasm-core-abi=f64 "$dir/no-main.atto"
reject wasm_f64_param 1 "native main does not admit f64 process ABI" --target=wasm32 --wasm-core-abi=f64 "$dir/param.atto"
reject duplicate_flag 2 "duplicate wasm-core-abi option" --target=wasm32 --wasm-core-abi=f64 --wasm-core-abi=f64 "$dir/float.atto"
reject invalid_flag 2 "unsupported wasm Core scalar ABI" --target=wasm32 --wasm-core-abi=i64 "$dir/float.atto"
[[ "$pass" -eq 10 ]] || { echo "WASM_F64_GATE_COUNT_MISMATCH:$pass" >&2; exit 1; }
echo "WASM_F64_SOURCE_IR_GATE_PASS=$pass"
