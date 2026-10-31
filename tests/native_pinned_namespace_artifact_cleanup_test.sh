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
# Simulate a file outside WORK_DIR. The harness may clean its own artifacts,
# but must never overwrite this external sentinel through an output symlink.
export SYNTH_SENTINEL="$root/external-sentinel"
printf 'external file must remain unchanged\n' >"$SYNTH_SENTINEL"
cat > "$root/helper" <<'HELPER'
#!/usr/bin/env bash
set -euo pipefail
case "${SYNTH_MODE:-normal}" in
  fail) echo 'synthetic helper failure' >&2; exit 1 ;;
  invalid-ir) printf 'not LLVM IR\n'; exit 0 ;;
  helper-stderr) echo 'synthetic helper warning' >&2 ;;
  symlink-ir) ln -s "${SYNTH_SENTINEL:?}" pinned-namespace.ll ;;
  symlink-native) ln -s "${SYNTH_SENTINEL:?}" pinned-namespace-native ;;
  noisy-guest)
    cat <<'LLVM_IR'
@.msg = private unnamed_addr constant [6 x i8] c"NOISE\00"
declare i32 @puts(ptr)
define i32 @main() {
entry:
  %line = getelementptr inbounds [6 x i8], ptr @.msg, i64 0, i64 0
  call i32 @puts(ptr %line)
  ret i32 43
}
LLVM_IR
    exit 0 ;;
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
expect_failure helper-stderr 43 '' 'pinned namespace helper wrote stderr'
expect_failure noisy-guest 43 '' 'pinned namespace guest produced output'
expect_failure normal 99 '' 'unexpected pinned namespace guest oracle'
expect_failure symlink-ir 43 '' 'namespace helper precreated output artifact'
expect_failure symlink-native 43 '' 'namespace helper precreated output artifact'
test "$(cat "$SYNTH_SENTINEL")" = 'external file must remain unchanged'
echo 'SYNTHETIC_NAMESPACE_HARNESS_CLEANUP=PASS positive=3 negative=8 scope=NOT_ORESLANG_COMPILER_OR_GUEST'
