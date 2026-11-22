#!/usr/bin/env bash
# Real LLVM native guest gate: no source concatenation, mocks or host algorithms.
# Requires compiler to resolve all three Atto file imports and execute main().
set -euo pipefail
if (( $# != 2 )); then
  echo "usage: $0 /path/to/oreslang-llvmc clang" >&2
  exit 64
fi
compiler="$1"
clang="$2"
test -x "$compiler"
command -v "$clang" >/dev/null
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/tests/stdlib" "$work/stdlib/std"
for path in \
  stdlib/std/websocket.atto \
  stdlib/std/quic.atto \
  stdlib/std/compute.atto \
  tests/stdlib/websocket_quic_compute_smoke.atto; do
  test -f "$path" && test ! -L "$path"
  cp -- "$path" "$work/$path"
  test "$(git hash-object -- "$path")" = "$(git hash-object -- "$work/$path")"
done

compile_ir() {
  local label="$1"
  local entry="$2"
  local ir="$work/$label.ll"
  "$compiler" --target=host "$entry" > "$ir" 2> "$work/$label.stderr" ||
    { echo "LLVM_ATTO_IMPORT_COMPILE_FAILED:$label" >&2; cat "$work/$label.stderr" >&2; exit 1; }
  test -s "$ir"
  grep -Fq '@main(' "$ir"
}
run_native() {
  local label="$1" expect="$2"
  for opt in 0 2; do
    local exe="$work/$label-O$opt"
    "$clang" "-O$opt" -x ir "$work/$label.ll" -o "$exe"
    local rc=0
    timeout 15s "$exe" > "$work/$label-O$opt.out" 2> "$work/$label-O$opt.err" || rc=$?
    if [ "$rc" -ne "$expect" ]; then
      echo "LLVM_ATTO_GUEST_WRONG_EXIT:$label expected=$expect got=$rc opt=$opt" >&2
      cat "$work/$label-O$opt.err" >&2
      exit 1
    fi
    test ! -s "$work/$label-O$opt.err"
    echo "ATTO_NATIVE_STD_GUEST_EXECUTED label=$label opt=O$opt exit=$rc"
  done
}

entry="$work/tests/stdlib/websocket_quic_compute_smoke.atto"
compile_ir positive "$entry"
run_native positive 0

# Tamper an exact .atto assertion; guest must fail with its own case number.
sed 's/quic_varint_octet(15293, 2, 0) != 123 then return 12; fi/quic_varint_octet(15293, 2, 0) != 124 then return 12; fi/' \
  "$entry" > "$work/tests/stdlib/mutated.atto"
grep -Fq '!= 124 then return 12; fi' "$work/tests/stdlib/mutated.atto"
compile_ir mutation "$work/tests/stdlib/mutated.atto"
run_native mutation 12

# Unknown imported function must be compiler rejection, never fallback.
sed 's/quic_encoding_min_width/quic_missing_min_width/g' \
  "$entry" > "$work/tests/stdlib/missing-symbol.atto"
grep -Fq 'quic_missing_min_width' "$work/tests/stdlib/missing-symbol.atto"
if "$compiler" --target=host "$work/tests/stdlib/missing-symbol.atto" \
    > "$work/unknown.ll" 2> "$work/unknown.stderr"; then
  echo LLVM_ATTO_MISSING_SYMBOL_ACCEPTED >&2
  exit 1
fi
test ! -s "$work/unknown.ll"
test -s "$work/unknown.stderr"

# Missing library file must never be treated as an ambient std capability.
mv "$work/stdlib/std/quic.atto" "$work/stdlib/std/quic.atto.unavailable"
if "$compiler" --target=host "$entry" \
    > "$work/missing-file.ll" 2> "$work/missing-file.stderr"; then
  echo LLVM_ATTO_MISSING_SOURCE_ACCEPTED >&2
  exit 1
fi
test ! -s "$work/missing-file.ll"
test -s "$work/missing-file.stderr"
echo "ATTO_NATIVE_STD_LLVM_GATE guest_cases=37 run_modes=O0,O2 negative_runtime=12 negative_imports=2"
