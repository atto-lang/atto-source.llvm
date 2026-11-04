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
# The exact maximum input must be admitted without writing diagnostics.
head -c 65536 /dev/zero > "$work/exact-bound.bin"
[[ "$(wc -c < "$work/exact-bound.bin")" == 65536 ]] ||
  { echo 'failed to create exact 65536-byte native stdin fixture' >&2; exit 1; }
"$good" < "$work/exact-bound.bin" >"$output" 2>"$errors"
[[ ! -s "$output" && ! -s "$errors" ]] ||
  { echo 'maximally bounded LLVM fuzz input did not pass' >&2; exit 1; }
bash "$builder" "$root/tests/fixtures/atto-fuzz-byte-mutant.atto" "$mutant"
set +e
printf '\x00' | "$mutant" >"$output" 2>"$errors"
status=$?
set -e
[[ "$status" == 77 && ! -s "$output" ]] ||
  { echo "mutated compiled LLVM guest escaped the property oracle (rc=$status)" >&2; exit 1; }
grep -Fq 'ATTO_LLVM_FUZZ_MISMATCH position=0 input=0 expected=1 got=2' "$errors" ||
  { echo 'compiled guest mutation failed to report a bounded byte discrepancy' >&2; exit 1; }
head -c 65537 /dev/zero > "$work/over-bound.bin"
[[ "$(wc -c < "$work/over-bound.bin")" == 65537 ]] ||
  { echo 'failed to create exact 65537-byte native stdin fixture' >&2; exit 1; }
set +e
"$good" < "$work/over-bound.bin" >"$output" 2>"$errors"
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
# Prevent silently linking a host-incompatible guest ABI. A compiler may
# legitimately accept these AttoLang source signatures in another context;
# this one-input native property adapter MUST refuse both.
for wrong in atto-fuzz-wrong-arity.atto atto-fuzz-wrong-return.atto; do
  set +e
  bash "$builder" "$root/tests/fixtures/$wrong" "$good" >"$output" 2>"$errors"
  status=$?
  set -e
  [[ "$status" != 0 ]] ||
    { echo "native fuzz target accepted incompatible guest ABI: $wrong" >&2; exit 1; }
  grep -Fq 'LLVM guest must define exactly one external fuzz_byte(i64) -> i64' "$errors" ||
    { echo "compiler rejected source before the ABI admission test: $wrong" >&2; exit 1; }
  cmp -s "$good" "$work/preserved" ||
    { echo "failed ABI gate overwrote the previous native executable: $wrong" >&2; exit 1; }
done
# The compiler must run, but the mandatory Rust no-GC IR audit MUST reject
# publication when the auditor fails. This is a deliberate negative mutation
# of the build gate, not proof of guest runtime safety.
cat > "$work/failing-ir-auditor" <<'AUDITOR'
#!/usr/bin/env bash
# Do not copy the compiler, IR, or source outside the test work dir.
printf 'ATTO_FAILING_IR_AUDIT_REACHED\\n' >&2
exit 7
AUDITOR
chmod 700 "$work/failing-ir-auditor"
export ORES_NO_GC_IR_AUDIT="$work/failing-ir-auditor"
set +e
bash "$builder" "$root/tests/fixtures/atto-fuzz-byte.atto" "$good" >"$output" 2>"$errors"
status=$?
set -e
[[ "$status" == 7 ]] ||
  { echo "native LLVM fuzz builder ignored a failing IR auditor or did not reach it (rc=$status)" >&2; exit 1; }
grep -Fqx 'ATTO_FAILING_IR_AUDIT_REACHED' "$errors" ||
  { echo 'LLVM compiler failed before mandatory IR auditor denial was tested' >&2; exit 1; }
cmp -s "$good" "$work/preserved" ||
  { echo 'failing IR audit overwrote previously published fuzz executable' >&2; exit 1; }
export ORES_NO_GC_IR_AUDIT="$auditor"

# Instrumented compiler-generated IR + native stdin harness execution.
# A clean exit alone does NOT prove ASan/UBSan were active: the separate
# deliberate known-bad sanitizer controls remain mandatory release gates.
ATTO_FUZZ_SANITIZERS=address,undefined bash "$builder" \
  "$root/tests/fixtures/atto-fuzz-byte.atto" "$work/native-fuzz-asan-ubsan"
printf '\x00\xff' | "$work/native-fuzz-asan-ubsan" >"$output" 2>"$errors"
[[ ! -s "$output" && ! -s "$errors" ]] ||
  { echo 'instrumented LLVM guest emitted unexpected sanitizer diagnostics' >&2; exit 1; }

if find "$work" -maxdepth 1 -name '.atto-fuzz-build.*' | grep -q .; then
  echo 'native fuzz builder leaked temporary IR or executable artifacts' >&2
  exit 1
fi
printf 'ATTO_LLVM_NATIVE_STDIN_FUZZ_PASS compiled_good=1 compiled_mutant=1 negative=6 asan_ubsan_guest=1 limit=65536\n'
