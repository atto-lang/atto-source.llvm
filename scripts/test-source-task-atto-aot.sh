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
# Compiler-semantic source rejections from the existing native source Task E2E corpus.
# Each must fail before a guest artifact is published. Never launch these.
for name in missing-await double-await producer-call second-task actor-task-no-pool; do
  path="$root/tests/fixtures/task-v1-negative/$name.atto"
  [[ -f "$path" && ! -L "$path" ]] || { echo "Missing negative fixture: $name" >&2; exit 78; }
  [[ "$(git -C "$root" rev-parse "HEAD:tests/fixtures/task-v1-negative/$name.atto")" == "$(git hash-object --no-filters "$path")" ]] || { echo "Negative fixture differs from HEAD: $name" >&2; exit 78; }
  case "$name" in
    missing-await) expected="must be awaited exactly once" ;;
    double-await) expected="requires await to immediately consume" ;;
    producer-call) expected="producer must be capture-free and scalar" ;;
    second-task) expected="supports exactly one source Task" ;;
    actor-task-no-pool) expected="native actor task blocks require actor-owned scheduler continuation lowering; foreign Task pool is forbidden" ;;
  esac
  artifact="$tmp/rejected-$name"
  code=0
  timeout 30s "$root/scripts/compile-aot.sh" "$path" "$artifact" > "$tmp/negative-$name.out" 2> "$tmp/negative-$name.err" || code=$?
  [[ "$code" != 0 && "$code" != 124 ]] || { echo "Unsafe Task admission succeeded or timed out: $name" >&2; exit 1; }
  grep -Fq "$expected" "$tmp/negative-$name.err" || { echo "Wrong compiler diagnostic for $name" >&2; cat "$tmp/negative-$name.err" >&2; exit 1; }
  [[ ! -e "$artifact" ]] || { echo "Invalid Task source published native guest: $name" >&2; exit 1; }
  printf 'TASK_I64_ATTO_NEGATIVE_PASS|head=%s|case=%s|no_artifact=yes\n' "$EXPECTED_HEAD" "$name"
done
