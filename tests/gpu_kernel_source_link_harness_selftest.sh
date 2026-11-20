#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
test_script="$PWD/tests/atto_gpu_kernel_source_link_admission.cmake"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fake="$work/synthetic-llvmc"
cat >"$fake" <<'SYNTHETIC_COMPILER'
#!/usr/bin/env bash
set -euo pipefail
case_dir="$(basename "$(dirname "$1")")"
if [[ ! "$case_dir" =~ ^kernel-link-(.*)-[0-9a-f]{24}$ ]]; then
  echo "unexpected source-link case: $case_dir" >&2
  exit 2
fi
name="${BASH_REMATCH[1]}"
code="GPU-LAUNCH-008"
case "$name" in
  unknown-kernel|duplicate-kernel|duplicate-param|nonvoid|missing-body|wrong-scalar-name)
    code="GPU-LAUNCH-001" ;;
  wrong-arity|invalid-type|read-mut|saxpy-signedness|saxpy-wrong-operator)
    code="GPU-LAUNCH-002" ;;
  forbidden-await|string-body|template-body|defer-body|throw-body|raise-body|panic-body|async-body|nb-body|using-body|console-body|sleep-body|select-body|readch-body|writech-body|spawn-body)
    code="GPU-LAUNCH-006" ;;
esac
case "$name" in
  saxpy-invalid-axis) code="GPU-LAUNCH-003" ;;
  saxpy-unguarded|saxpy-readonly-store|saxpy-read-from-output|saxpy-wrong-index|saxpy-early-no-return|saxpy-wrong-bound)
    code="GPU-LAUNCH-005" ;;
esac
mode="${GPU_LINK_SELFTEST_MODE:-normal}"
if [[ "$name" == "unknown-kernel" ]]; then
  case "$mode" in
    incorrect-diagnostic) code="GPU-LAUNCH-008" ;;
    missing-prefix) echo "$code: no implicit CPU fallback" >&2; exit 1 ;;
    reflection) echo "source: $code: no implicit CPU fallback" >&2; exit 1 ;;
    missing-no-fallback) echo "oreslang-llvmc: $code: body unsupported" >&2; exit 1 ;;
    false-success) exit 0 ;;
    hidden-output) mkdir -p .hidden/sub; echo forged > .hidden/sub/kernel.ll ;;
    nested-output) mkdir -p nested/sub; echo forged > nested/sub/kernel.o ;;
    mutated-source) echo changed >> "$1" ;;
    stdout-output) echo "forged LLVM IR" ;;
    signaled) kill -TERM "$BASHPID" ;;
  esac
fi
echo "oreslang-llvmc: $code: no implicit CPU fallback" >&2
exit 1
SYNTHETIC_COMPILER
chmod 700 "$fake"
run() {
  local mode="$1"
  GPU_LINK_SELFTEST_MODE="$mode" cmake -DCOMPILER="$fake" \
    -DWORKDIR="$work/$mode" -P "$test_script" >"$work/$mode.log" 2>&1
}
if ! run normal; then
  cat "$work/normal.log" >&2
  echo "GPU_LINK_HARNESS_FAIL: normal synthetic fixture rejected" >&2
  exit 1
fi
count=1
for case_name in incorrect-diagnostic missing-prefix reflection missing-no-fallback false-success hidden-output nested-output mutated-source stdout-output signaled; do
  if run "$case_name"; then
    echo "GPU_LINK_HARNESS_FAIL: accepted forged evidence $case_name" >&2
    exit 1
  fi
  if ! grep -q 'CMake Error' "$work/$case_name.log"; then
    echo "GPU_LINK_HARNESS_FAIL: unexpected error class $case_name" >&2
    cat "$work/$case_name.log" >&2
    exit 1
  fi
  count=$((count+1))
done
echo "PASS $count source-free kernel-link evidence scenarios; real_llvmc=false; physical_gpu=false"
