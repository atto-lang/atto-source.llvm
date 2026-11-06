#!/usr/bin/env bash
# Exact-head real .atto Task<int> -> IR -> native AOT O0/O2.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?Pin the exact LLVM source commit}"
[[ "$EXPECTED_HEAD" =~ ^[a-f0-9]{40}$ ]] || exit 64
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || { echo 'wrong source HEAD' >&2; exit 78; }
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
[[ -x "$compiler" && -x "$auditor" ]] || { echo 'Build compiler and no-GC IR auditor first' >&2; exit 69; }
command -v clang >/dev/null || { echo 'Clang missing' >&2; exit 69; }
command -v timeout >/dev/null || { echo 'timeout missing' >&2; exit 69; }
source="$root/tests/fixtures/source-task-i64-await.atto"
legacy="$root/tests/fixtures/source-task-i64-await.ores"
cmp -s "$source" "$legacy" || { echo 'Task source changed from canonical test fixture' >&2; exit 78; }
tmp="$(mktemp -d "${TMPDIR:-/tmp}/atto-task-llvm.XXXXXXXX")"
trap 'rm -rf -- "$tmp"' EXIT
cp -- "$source" "$tmp/pristine.atto"
fixture_sha="$(sha256sum "$source" | cut -d' ' -f1)"
compiler_sha="$(sha256sum "$compiler" | cut -d' ' -f1)"
for level in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
  "$compiler" --target=host "$source" > "$tmp/program-$level.ll"
  for symbol in oreslang_source_task_start_i64 oreslang_source_task_await_i64 oreslang_source_task_runtime_shutdown; do
    grep -Fq "@$symbol" "$tmp/program-$level.ll" || { echo "Missing native Task ABI $symbol" >&2; exit 1; }
  done
  "$auditor" "$tmp/program-$level.ll" >/dev/null
  executable="$tmp/guest-$level"
  "$root/scripts/compile-aot.sh" "$source" "$executable" > "$tmp/build-$level.txt"
  [[ -x "$executable" && ! -L "$executable" ]] || { echo "O$level native guest missing" >&2; exit 1; }
  result=0
  timeout 15s "$executable" > "$tmp/stdout-$level" 2> "$tmp/stderr-$level" || result=$?
  [[ "$result" == 42 ]] || { echo "O$level guest exited $result" >&2; cat "$tmp/stderr-$level" >&2; exit 1; }
  cmp -s "$source" "$tmp/pristine.atto" || { echo 'Source changed during execution' >&2; exit 1; }
  printf 'TASK_I64_ATTO_GUEST_PASS|head=%s|O%s|exit=42|compiler_sha256=%s|fixture_sha256=%s|binary_sha256=%s\n' \
      "$EXPECTED_HEAD" "$level" "$compiler_sha" "$fixture_sha" "$(sha256sum "$executable" | cut -d' ' -f1)"
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
cat > "$tmp/missing-await.atto" <<'SOURCE'
pub fnc main(): int {
  let Task<int> w = task { return 41; };
  return 0;
}
SOURCE
if "$root/scripts/compile-aot.sh" "$tmp/missing-await.atto" "$tmp/negative" > "$tmp/negative.out" 2> "$tmp/negative.err"; then
  echo 'Invalid Task source admitted' >&2; exit 1
fi
grep -Fq 'must be awaited exactly once' "$tmp/negative.err" || { cat "$tmp/negative.err" >&2; exit 1; }
[[ ! -e "$tmp/negative" ]] || { echo 'Invalid source published an executable' >&2; exit 1; }
printf 'TASK_I64_ATTO_NEGATIVE_PASS|head=%s|no_artifact=yes\n' "$EXPECTED_HEAD"
