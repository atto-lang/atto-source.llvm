#!/usr/bin/env bash
# Actual native Oreslang compiler -> LLVM IR -> OS executable zero-assert gate.
# Five positive pure guest programs plus one removed-operator rejection.
# This does NOT prove ores.std/testing module/class/receipt support.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
compiler="${1:-build/oreslang-llvmc}"
[[ -x "$compiler" ]] || { echo "missing executable native LLVM compiler" >&2; exit 2; }
command -v clang >/dev/null || { echo "clang unavailable" >&2; exit 2; }
fixtures="contracts/parity/native-scalar-zero-v1"
manifest="$fixtures/cases.tsv"
[[ "$(git hash-object "$manifest")" == a87f64af3e448ba10927eb2932900679896b283b ]] ||
  { echo "Oreslang scalar fixture manifest mismatch" >&2; exit 1; }
verify_fixture() {
  local name="$1" pinned
  pinned="$(awk -F '\t' -v name="$name" '$1==name {print $3}' "$manifest")"
  [[ "$pinned" =~ ^[0-9a-f]{40}$ && "$(git hash-object "$fixtures/$name")" == "$pinned" ]] ||
    { echo "native Oreslang fixture byte identity mismatch: $name" >&2; exit 1; }
}
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tested=0
for name in \
  precedence-zero.ores \
  i64-precision-zero.ores \
  signed-opposite-zero.ores \
  nested-subtraction-zero.ores \
  min-boundary-zero.ores
do
  verify_fixture "$name"
  source="$fixtures/$name"
  [[ -f "$source" && ! -L "$source" ]] || { echo "missing fixture: $name" >&2; exit 1; }
  ir="$work/$name.ll"
  exe="$work/$name.bin"
  "$compiler" "$source" > "$ir" 2> "$work/$name.compiler-stderr" || {
    echo "compiler rejected valid Oreslang test: $name" >&2; exit 1;
  }
  [[ -s "$ir" ]] || { echo "empty LLVM IR: $name" >&2; exit 1; }
  clang -x ir "$ir" -o "$exe"
  "$exe" > "$work/$name.stdout" 2> "$work/$name.stderr" || {
    echo "nonzero native Oreslang assertion result: $name" >&2; exit 1;
  }
  [[ ! -s "$work/$name.stdout" && ! -s "$work/$name.stderr" ]] ||
    { echo "unexpected guest output: $name" >&2; exit 1; }
  tested=$((tested+1))
done
verify_fixture "reject-removed-operator.ores"
invalid="$fixtures/reject-removed-operator.ores"
if "$compiler" "$invalid" > "$work/invalid.ll" 2> "$work/invalid.stderr"; then
  echo "removed % source operator erroneously admitted" >&2
  exit 1
fi
[[ ! -s "$work/invalid.ll" && -s "$work/invalid.stderr" ]] ||
  { echo "negative source must fail closed without IR" >&2; exit 1; }
# Valid Oreslang with a deliberately incorrect expected value must execute
# with nonzero status; this proves that an assertion mutation cannot pass.
printf 'pub fnc main():int{return (2 + 3 * 4) - 13;}\n' > "$work/mutant.ores"
"$compiler" "$work/mutant.ores" > "$work/mutant.ll" 2> "$work/mutant.compiler-stderr"
[[ -s "$work/mutant.ll" ]] || { echo "mutant produced no LLVM IR" >&2; exit 1; }
clang -x ir "$work/mutant.ll" -o "$work/mutant.bin"
if "$work/mutant.bin" > "$work/mutant.stdout" 2> "$work/mutant.stderr"; then mutant_status=0; else mutant_status=$?; fi
[[ "$mutant_status" -eq 1 && ! -s "$work/mutant.stdout" && ! -s "$work/mutant.stderr" ]] ||
  { echo "native Oreslang nonzero mutation was not detected" >&2; exit 1; }
[[ "$tested" -eq 5 ]]
echo "LLVM_NATIVE_SCALAR_ZERO executed=5 rejected=1 full_std_test_harness=unverified"
