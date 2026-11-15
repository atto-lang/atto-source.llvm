#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tests="$root/tests/atto_gpu_explicit_launch_admission.cmake"
if [[ ! -f "$tests" ]]; then
  echo "GPU_HARNESS_SELFTEST_FAIL: missing native admission test" >&2
  exit 1
fi
workspace="$(mktemp -d)"
trap 'rm -rf "$workspace"' EXIT
compiler="$workspace/synthetic-native-compiler"
cat > "$compiler" <<'COMPILER'
#!/usr/bin/env bash
set -euo pipefail
input="$1"
case_dir="$(basename "$(dirname "$input")")"
if [[ "$case_dir" == scalar-control ]]; then
  exit 0
fi
if [[ ! "$case_dir" =~ ^case-(.+)-[0-9a-f]{20}$ ]]; then
  echo 'synthetic compiler: unexpected case directory' >&2
  exit 2
fi
case_name="${BASH_REMATCH[1]}"
code=GPU-LAUNCH-008
case "$case_name" in
  invalid-block|zero-grid|negative-grid|host-geometry|zero-divisor|mixed-geometry|zero-logical-group|overflow-grid|negative-shared|physical-2d|extra-axes|unknown-field|missing-queue) code=GPU-LAUNCH-003 ;;
  wrong-placement|empty-device) code=GPU-LAUNCH-004 ;;
  literal-argument) code=GPU-LAUNCH-002 ;;
esac
mode="${GPU_SELFTEST_MODE:-baseline}"
if [[ "$case_name" == invalid-block ]]; then
  case "$mode" in
    wrong-diagnostic) code=GPU-LAUNCH-008 ;;
    artifact-hidden) mkdir -p .hidden; printf 'bad' > .hidden/out.ll ;;
    artifact-nested) mkdir -p obj; printf 'bad' > obj/out.ll ;;
    stdout) printf 'bad-guest-output\n' ;;
    mutation) printf 'changed\n' >> "$input" ;;
    no-fallback) echo "oreslang-llvmc: $code: no provider" >&2; exit 1 ;;
    zero-exit) exit 0 ;;
    signal-crash) kill -TERM "$BASHPID" ;;
    reflection) echo "source: GPU-LAUNCH-003: no implicit CPU fallback" >&2; exit 1 ;;
    stderr-only-code) echo "GPU-LAUNCH-003: no implicit CPU fallback" >&2; exit 1 ;;
  esac
fi
echo "oreslang-llvmc: $code: no implicit CPU fallback" >&2
exit 1
COMPILER
chmod 700 "$compiler"

run() {
  local mode="$1"
  GPU_SELFTEST_MODE="$mode" cmake -DCOMPILER="$compiler" \
    -DWORKDIR="$workspace/$mode" -P "$tests" > "$workspace/$mode.log" 2>&1
}

run baseline || { cat "$workspace/baseline.log" >&2; exit 1; }
passed=1
for mode in wrong-diagnostic artifact-hidden artifact-nested stdout mutation no-fallback zero-exit reflection stderr-only-code signal-crash; do
  if run "$mode"; then
    echo "GPU_HARNESS_SELFTEST_FAIL: accepted corrupted $mode evidence" >&2
    exit 1
  fi
  if ! grep -q '^CMake Error' "$workspace/$mode.log"; then
    echo "GPU_HARNESS_SELFTEST_FAIL: missing fatal CMake error for $mode" >&2
    cat "$workspace/$mode.log" >&2
    exit 1
  fi
  ((passed+=1))
done
printf 'PASS %d source-free negative-gate mutation scenarios; native_compiler_executed=false; gpu_executed=false\n' "$passed"
