#!/usr/bin/env bash
# Compiler-backed source admission only: f64 literals must NOT produce i64 IR.
# This is not f64 codegen or a floating-point parity test.
set -euo pipefail
umask 077
export LC_ALL=C
[[ "$#" -eq 2 ]] || { echo "usage: native_f64_lexical_gate.sh <llvmc> <clang>" >&2; exit 64; }
compiler="$1"; clang="$2"
[[ -x "$compiler" ]] && command -v "$clang" >/dev/null ||
  { echo "Missing real LLVM compiler or Clang" >&2; exit 69; }
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
checks=0

negative() {
  local name="$1" source="$2" diagnostic="$3"
  printf '%s\n' "$source" > "$scratch/$name.ores"
  if "$compiler" --target=host "$scratch/$name.ores" \
      > "$scratch/$name.ll" 2> "$scratch/$name.stderr"; then
    echo "NATIVE_F64_FALSE_ADMISSION: $name" >&2; exit 1
  fi
  [[ ! -s "$scratch/$name.ll" ]] ||
    { echo "NATIVE_F64_PARTIAL_IR: $name" >&2; exit 1; }
  grep -Fq -- "$diagnostic" "$scratch/$name.stderr" ||
    { echo "NATIVE_F64_MISSING_DIAGNOSTIC: $name" >&2
      cat "$scratch/$name.stderr" >&2; exit 1; }
  checks=$((checks+1))
  echo "NATIVE_F64_UNSUPPORTED_EXPLICIT: $name"
}

# Positive control ensures the compiler isn't simply failing everything.
printf '%s\n' 'pub fnc main(): int { return 42; }' > "$scratch/i64.ores"
"$compiler" --target=host "$scratch/i64.ores" \
  > "$scratch/i64.ll" 2> "$scratch/i64.compiler.stderr"
[[ -s "$scratch/i64.ll" && ! -s "$scratch/i64.compiler.stderr" ]] ||
  { echo 'NATIVE_F64_CONTROL_REJECTED' >&2; exit 1; }
"$clang" -x ir "$scratch/i64.ll" -o "$scratch/i64.native" \
  > "$scratch/i64.link.stdout" 2> "$scratch/i64.link.stderr"
[[ -x "$scratch/i64.native" ]] ||
  { echo 'NATIVE_F64_CONTROL_LINK_FAILED' >&2; exit 1; }
rc=0
"$scratch/i64.native" > "$scratch/i64.stdout" 2> "$scratch/i64.stderr" || rc=$?
[[ "$rc" -eq 42 && ! -s "$scratch/i64.stdout" &&
   ! -s "$scratch/i64.stderr" ]] ||
  { echo 'NATIVE_F64_CONTROL_GUEST_FAILED' >&2; exit 1; }
checks=$((checks+1))
echo 'NATIVE_I64_POSITIVE_CONTROL_EXECUTED'

negative decimal \
  'pub fnc main(): int { return 0.3; }' \
  'native f64 literal requires typed IEEE-754 lowering; unsupported'
negative scientific \
  'pub fnc main(): int { return 1e2; }' \
  'native f64 literal requires typed IEEE-754 lowering; unsupported'
negative signed_exponent \
  'pub fnc main(): int { return 1E-2; }' \
  'native f64 literal requires typed IEEE-754 lowering; unsupported'
negative tiny_fraction \
  'pub fnc main(): int { return 0.000000001; }' \
  'native f64 literal requires typed IEEE-754 lowering; unsupported'
negative malformed_exponent \
  'pub fnc main(): int { return 1e+; }' \
  'malformed native floating exponent at byte'

[[ "$checks" -eq 6 ]] ||
  { echo "NATIVE_F64_INCOMPLETE_TEST_COUNT=$checks" >&2; exit 1; }
echo 'NATIVE_F64_GATING: i64 control 1/1; five negative cases (no native f64 codegen)'
