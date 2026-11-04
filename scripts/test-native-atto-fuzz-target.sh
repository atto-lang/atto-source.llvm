#!/usr/bin/env bash
# Compiler-backed native stdin fuzz parity using LLVM-generated .atto guest.
# No fake compiler, no synthetic guest AST, no Python, no private source mirror.
set -euo pipefail
(( $# == 3 )) ||
  { echo 'usage: test-native-atto-fuzz-target.sh LLVM_COMPILER CLANG IR_AUDITOR' >&2; exit 64; }
compiler="$1"
clang="$2"
auditor="$3"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
builder="$root/scripts/build-native-atto-fuzz-target.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
export ORESLANG_LLVMC="$compiler"
export ORES_NO_GC_IR_AUDIT="$auditor"
export CLANG="$clang"
good="$work/native-fuzz"
mutant="$work/native-fuzz-mutant"
output="$work/result.stdout"
errors="$work/result.stderr"
bash "$builder" "$root/tests/fixtures/atto-fuzz-byte.atto" "$good"
printf '\x00\x01\x7f\xff' | "$good" >"$output" 2>"$errors"
[[ ! -s "$output" && ! -s "$errors" ]] ||
  { echo 'correct compiled LLVM guest had unexpected output' >&2; exit 1; }
"$good" </dev/null >"$output" 2>"$errors"
[[ ! -s "$output" && ! -s "$errors" ]] ||
  { echo 'empty input should be a clean no-op' >&2; exit 1; }
bash "$builder" "$root/tests/fixtures/atto-fuzz-byte-mutant.atto" "$mutant"
set +e
printf '\x00' | "$mutant" >"$output" 2>"$errors"
status=$?
set -e
[[ "$status" == 77 && ! -s "$output" ]] ||
  { echo "mutated compiled LLVM guest escaped the property oracle (rc=$status)" >&2; exit 1; }
grep -Fq 'ATTO_LLVM_FUZZ_MISMATCH position=0 input=0 expected=1 got=2' "$errors" ||
  { echo 'compiled guest mutation failed to report a bounded byte discrepancy' >&2; exit 1; }
set +e
head -c 65537 /dev/zero | "$good" >"$output" 2>"$errors"
status=$?
set -e
[[ "$status" == 2 && ! -s "$output" ]] ||
  { echo 'native stdin adapter accepted over-budget input' >&2; exit 1; }
grep -Fq 'ATTO_LLVM_FUZZ_INCONCLUSIVE input budget exceeded' "$errors" ||
  { echo 'native byte budget missing diagnostic' >&2; exit 1; }
cp "$good" "$work/preserved"
set +e
bash "$builder" "$root/tests/fixtures/atto-test-unsupported-call.atto" "$good" >"$output" 2>"$errors"
status=$?
set -e
[[ "$status" != 0 ]] ||
  { echo 'malformed LLVM guest rebuilt target unexpectedly' >&2; exit 1; }
cmp -s "$good" "$work/preserved" ||
  { echo 'failed compiler admission overwrote the previous runnable target' >&2; exit 1; }
if find "$work" -maxdepth 1 -name '.atto-fuzz-build.*' | grep -q .; then
  echo 'native fuzz builder leaked temporary IR or executable artifacts' >&2
  exit 1
fi
printf 'ATTO_LLVM_NATIVE_STDIN_FUZZ_PASS compiled_good=1 compiled_mutant=1 negative=3 limit=65536\n'
