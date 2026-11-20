#!/usr/bin/env bash
# Source-owned compiler-backed AttoLang O0/O2 parity. Never substitute synthetic IR.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$root"
: "$EXPECTED_HEAD" "$ORESLANG_LLVMC" "$CLANG"
[[ -n "$EXPECTED_HEAD" && -n "$ORESLANG_LLVMC" && -n "$CLANG" ]] || exit 64
[[ "$(git rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || { echo 'wrong checkout' >&2; exit 64; }
[[ -z "$(git status --porcelain)" ]] || { echo 'dirty source' >&2; exit 64; }
compiler="$(realpath -e -- "$ORESLANG_LLVMC")"
[[ -x "$compiler" && "$(basename "$compiler")" == oreslang-llvmc ]] || exit 64
command -v -- "$CLANG" > /dev/null || exit 69
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
printf 'ATTO_LLVM_HEAD=%s|tree=%s|compiler_sha256=%s\n' "$EXPECTED_HEAD" "$(git rev-parse 'HEAD^{tree}')" "$(sha256sum "$compiler" | cut -d' ' -f1)"
set -- "contracts/atto-parity-v1/add.atto" "tests/fixtures/numerics-atto-main.atto"
n=0
for relative in "$@"; do
  input="$root/$relative"
  [[ -f "$input" && ! -L "$input" && "$input" == *.atto ]] || { echo "invalid fixture $relative" >&2; exit 1; }
  cp -- "$input" "$tmp/pristine-$n"
  source_hash="$(sha256sum "$input" | cut -d' ' -f1)"
  for level in 0 2; do
    # Bind the frontend probe to the same O-level as native AOT compilation.
    # Otherwise the comparison can silently measure the previous iteration.
    export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
    ir="$tmp/program-$n-$level.ll"
    "$compiler" --target=host "$input" > "$ir"
    [[ -s "$ir" ]] && grep -Eq '^define .*@' "$ir" || { echo 'missing native IR' >&2; exit 1; }
    executable="$tmp/native-$n-$level"
    "$root/scripts/compile-aot.sh" "$input" "$executable" > "$tmp/build-$n-$level.log"
    [[ -x "$executable" ]] || { echo 'missing executable' >&2; exit 1; }
    [[ "$(head -c 4 "$executable" | od -An -tx1 | tr -d ' \n')" == 7f454c46 ]] || { echo 'not ELF' >&2; exit 1; }
    code=0
    timeout --signal=TERM 15s "$executable" > "$tmp/out-$n-$level" 2> "$tmp/err-$n-$level" || code=$?
    [[ "$code" == 42 ]] || { echo "guest O$level $relative exited $code" >&2; exit 1; }
    cmp -s "$input" "$tmp/pristine-$n" || { echo 'source mutated' >&2; exit 1; }
  done
  cmp -s "$tmp/program-$n-0.ll" "$tmp/program-$n-2.ll" || { echo 'frontend IR not deterministic across Clang options' >&2; exit 1; }
  # Emit success only after the complete O0/O2 pair passes the IR parity gate.
  for level in 0 2; do
    ir="$tmp/program-$n-$level.ll"
    executable="$tmp/native-$n-$level"
    printf 'ATTO_LLVM_NATIVE_GUEST_PASS|head=%s|source=%s|O%s|exit=42|source_sha256=%s|ir_sha256=%s|binary_sha256=%s\n' "$EXPECTED_HEAD" "$relative" "$level" "$source_hash" "$(sha256sum "$ir" | cut -d' ' -f1)" "$(sha256sum "$executable" | cut -d' ' -f1)"
  done
  n=$((n + 1))
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
cp -- "$root/contracts/atto-parity-v1/add.atto" "$tmp/valid.atto"
"$compiler" --target=host "$tmp/valid.atto" > "$tmp/valid.ll"
grep -Eq '^define .*@' "$tmp/valid.ll" || { echo 'temp-source acceptance control failed' >&2; exit 1; }
printf '%s\n' 'pub fnc main(): int { return 40 + ; }' > "$tmp/invalid.atto"
for level in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
  if "$root/scripts/compile-aot.sh" "$tmp/invalid.atto" "$tmp/rejected-$level" > "$tmp/rejected.stdout" 2> "$tmp/rejected.stderr"; then
    echo 'invalid .atto compiled' >&2; exit 1
  fi
  [[ ! -e "$tmp/rejected-$level" ]] || { echo 'invalid guest published artifact' >&2; exit 1; }
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
printf 'ATTO_LLVM_GUEST_MATRIX_PASS|head=%s|fixtures=2|executions=4|negative_no_artifact=2|synthetic_ir=NO\n' "$EXPECTED_HEAD"
