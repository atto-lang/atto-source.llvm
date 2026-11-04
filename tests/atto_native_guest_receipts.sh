#!/usr/bin/env bash
# Actual .atto guest -> real LLVM IR -> Clang O0/O2 -> native stdout+exit.
# Native guest owns ORES_TEST output; the shell only observes/verifies it.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
clang="${CLANG:-clang}"
corpus="$root/tests/atto-native-harness/receipts"
die() { echo "FAIL LLVM guest receipt smoke: $*" >&2; exit 1; }
[[ -x "$compiler" && ! -L "$compiler" ]] || die "missing compiler"
clang_exe="$(command -v "$clang")" || die "missing Clang"
[[ -x "$clang_exe" ]] || die "Clang is not executable"
command -v timeout >/dev/null || die "timeout unavailable"
command -v python3 >/dev/null || die "python3 unavailable"
command -v sha256sum >/dev/null || die "sha256sum unavailable"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
[[ -x "$auditor" && ! -L "$auditor" ]] || die "missing mandatory no-GC IR auditor"
runtime="$root/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
verifier="$root/tests/verify_atto_guest_receipts.py"
[[ -f "$runtime" && ! -L "$runtime" && -f "$verifier" && ! -L "$verifier" ]] ||
  die "untrusted or missing native runtime/verifier"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
declare -A original_hash
for source in "$corpus/pass.atto" "$corpus/fail.atto" "$corpus/multi-pass.atto" "$corpus/multi-fail.atto" \
    "$corpus/bounded_testkit.atto" "$corpus/bounded-pp.atto" "$corpus/bounded-pf.atto" \
    "$corpus/bounded-fp.atto" "$corpus/bounded-ff.atto" \
    "$corpus/m1-pass.atto" "$corpus/m1-fail-fast.atto" "$corpus/m1-filter-skip.atto" \
    "$corpus/m1-skipped.atto" "$corpus/m1-filtered.atto" "$corpus/m1-invariants.atto" \
    "$corpus/m2-pass.atto" "$corpus/m2-fail-fast.atto" "$corpus/m2-filter-skip.atto" \
    "$corpus/m2-invalid-recover.atto" "$corpus/m2-duplicate-recover.atto" "$corpus/m2-once.atto" \
    "$root/stdlib/testing_m2.atto" \
    "$root/stdlib/testing_m1.atto" "$root/src/main/cpp/oreslang/frontend/CoreLibrarySources.hpp" \
    "$runtime" "$verifier"; do
  [[ -f "$source" && ! -L "$source" ]] || die "source absent or symlinked: $source"
  original_hash["$source"]="$(sha256sum "$source" | cut -d' ' -f1)"
done
python3 "$verifier" --self-test
PYTHONOPTIMIZE=2 python3 "$verifier" --self-test
for kind in pass fail multi-pass multi-fail bounded-pp bounded-pf bounded-fp bounded-ff \
    m1-pass m1-fail-fast m1-filter-skip m1-skipped m1-filtered m1-invariants \
    m2-pass m2-fail-fast m2-filter-skip m2-invalid-recover m2-duplicate-recover m2-once; do
  source="$corpus/$kind.atto"
  expected=0
  case "$kind" in
    fail|multi-fail|bounded-pf|bounded-fp|bounded-ff|m1-fail-fast|m1-skipped|m1-filtered|m2-fail-fast|m2-filter-skip)
      expected=1 ;;
  esac
  for level in 0 2; do
    ir="$tmp/$kind-O$level.ll"
    exe="$tmp/$kind-O$level.exe"
    set +e
    timeout 60s "$compiler" --target=host "$source" > "$ir" 2> "$tmp/compile.err"
    compiled=$?
    set -e
    [[ "$compiled" -eq 0 && -s "$ir" ]] ||
      { head -c 4096 "$tmp/compile.err" >&2; die "guest compilation failed $kind O$level status=$compiled"; }
    grep -Eq 'define[[:space:]]+i32[[:space:]]+@main\(' "$ir" ||
      die "native main missing: $kind O$level"
    grep -Eq '@ores_string_print\(' "$ir" ||
      die "native guest print lowering missing: $kind O$level"
    timeout 20s "$auditor" "$ir" > /dev/null 2> "$tmp/no-gc-audit.err" ||
      { head -c 4096 "$tmp/no-gc-audit.err" >&2; die "no-GC IR audit failed $kind O$level"; }
    timeout 60s "$clang_exe" "-O$level" -x ir "$ir" -x c -std=c11 \
      -I "$root/src/main/cpp" "$runtime" -pthread -o "$exe" \
      > "$tmp/clang.stdout" 2> "$tmp/clang.stderr" ||
      { head -c 4096 "$tmp/clang.stderr" >&2; die "Clang guest linking failed $kind O$level"; }
    [[ -f "$exe" && -x "$exe" && ! -L "$exe" ]] ||
      die "native guest executable missing: $kind O$level"
    # NativeStringRegion uses a nonblocking stdout reactor: the guest MUST
    # see a pipe, not a regular file. The shell captures the producer's status.
    set +e
    timeout 10s "$exe" 2> "$tmp/guest.stderr" | cat > "$tmp/guest.stdout"
    statuses=("${PIPESTATUS[@]}")
    set -e
    [[ "${statuses[0]}" -eq "$expected" && "${statuses[1]}" -eq 0 ]] ||
      die "native guest status mismatch: $kind O$level expected=$expected got=${statuses[*]}"
    [[ ! -s "$tmp/guest.stderr" ]] ||
      die "native guest unexpected stderr: $kind O$level"
    python3 "$verifier" "$tmp/guest.stdout" "$kind" "${statuses[0]}" ||
      die "guest-owned receipt stream invalid $kind O$level"
    printf 'ATTO_LLVM_GUEST_RECEIPT_HOST_OBSERVED|%s|O%s|exit=%s|guest=executed|receipt=guest_stdout\n' \
      "$kind" "$level" "$expected"
  done
  cmp -s "$tmp/$kind-O0.ll" "$tmp/$kind-O2.ll" ||
    die "native IR changed between O0/O2 toolchain executions: $kind"
done
for source in "${!original_hash[@]}"; do
  [[ "$(sha256sum "$source" | cut -d' ' -f1)" == "${original_hash["$source"]}" ]] ||
    die "guest input mutated during compiler execution: $source"
done
echo 'ATTO_LLVM_GUEST_RECEIPT_SMOKE_EXECUTED|suites=20|optimizations=2|native=40|testkit=PARTIAL_M2'
