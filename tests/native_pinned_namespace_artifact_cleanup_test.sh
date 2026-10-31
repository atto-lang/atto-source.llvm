#!/usr/bin/env bash
# Source-free CMake harness regression. Synthetic LLVM IR is NOT Oreslang guest evidence.
set -euo pipefail
if [[ $# -ne 2 || ! -f "$1" || ! -x "$2" ]]; then
  echo 'usage: native_pinned_namespace_artifact_cleanup_test.sh <harness.cmake> <clang>' >&2
  exit 2
fi
harness="$1"
clang="$2"
root="$(mktemp -d)"
trap 'rm -rf "$root"' EXIT
work="$root/work"
mkdir -p "$work"
cat > "$root/helper" <<'HELPER'
#!/usr/bin/env bash
set -euo pipefail
case "${SYNTH_MODE:-normal}" in
  fail) echo 'synthetic helper failure' >&2; exit 1 ;;
  invalid-ir) printf 'not LLVM IR\n'; exit 0 ;;
  *) ;;
esac
value=43
if [[ "${1:-}" == '--all-providers' ]]; then value=53; fi
if [[ "${SYNTH_MODE:-normal}" == 'wrong-exit' ]]; then value=43; fi
printf 'define i32 @main() {\nentry:\n  ret i32 %s\n}\n' "$value"
HELPER
chmod 700 "$root/helper"
run_harness() {
  cmake "-DHELPER=$root/helper" "-DCLANG=$clang" "-DWORK_DIR=$work" \
    "-DEXPECTED_GUEST_STATUS=$1" "-DHELPER_ARGS=$2" -P "$harness"
}
artifacts_exist() {
  [[ -f "$work/pinned-namespace.ll" && -x "$work/pinned-namespace-native" ]]
}
artifacts_absent() {
  [[ ! -e "$work/pinned-namespace.ll" && ! -L "$work/pinned-namespace.ll" &&
     ! -e "$work/pinned-namespace-native" && ! -L "$work/pinned-namespace-native" ]]
}
expect_failure() {
  local mode="$1" expected="$2" args="$3" diagnostic="$4"
  if SYNTH_MODE="$mode" run_harness "$expected" "$args" >"$root/rejection.txt" 2>&1; then
    echo "synthetic harness unexpectedly accepted: $mode" >&2
    exit 1
  fi
  grep -Fq "$diagnostic" "$root/rejection.txt"
  artifacts_absent || { echo "stale artifact retained after $mode" >&2; exit 1; }
}
run_harness 43 '' >/dev/null
artifacts_exist
expect_failure fail 43 '' 'pinned namespace helper failed'
run_harness 53 '--all-providers' >/dev/null
artifacts_exist
expect_failure wrong-exit 53 '--all-providers' 'pinned namespace guest exit mismatch'
run_harness 43 '' >/dev/null
artifacts_exist
expect_failure invalid-ir 43 '' 'did not emit a native main'
echo 'SYNTHETIC_NAMESPACE_HARNESS_CLEANUP=PASS positive=3 negative=3 scope=NOT_ORESLANG_COMPILER_OR_GUEST'
