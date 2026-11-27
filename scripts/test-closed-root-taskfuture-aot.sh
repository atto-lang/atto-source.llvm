#!/usr/bin/env bash
# Closed root TaskFuture/Future source admission; not general actor continuation support.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?pin exact source head}"
[[ "$EXPECTED_HEAD" =~ ^[a-f0-9]{40}$ && "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]]
[[ -z "$(git -C "$root" status --porcelain --untracked-files=no)" ]]
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
[[ -x "$compiler" && -x "$auditor" ]]
export ORESLANG_LLVMC="$compiler" ORES_NO_GC_IR_AUDIT="$auditor"
export ORESLANG_AOT_LTO=off ORESLANG_AOT_OBJECT_PIPELINE=direct ORESLANG_LLVM_AOT_PROFILE=off
command -v "${CLANG:-clang}" >/dev/null
command -v timeout >/dev/null
compiler_sha="$(sha256sum "$compiler" | cut -d' ' -f1)"
fixture_dir="$root/tests/fixtures/portable-task-v2-pilot"
# These are the original five shared Git blob identities, not backend rewrites.
[[ ! -L "$fixture_dir/taskfuture-int.atto" && "$(git hash-object --no-filters "$fixture_dir/taskfuture-int.atto")" == "32aa66ebc3ee9418d1da088393bb66762a67a881" ]]
[[ ! -L "$fixture_dir/future-int.atto" && "$(git hash-object --no-filters "$fixture_dir/future-int.atto")" == "63d69d589614250aa191510fee2990e7f3499aef" ]]
[[ ! -L "$fixture_dir/taskfuture-string.atto" && "$(git hash-object --no-filters "$fixture_dir/taskfuture-string.atto")" == "29eea1fc75ae3bd79a7156a06efa3318a94a4c63" ]]
[[ ! -L "$fixture_dir/reject-double-await.atto" && "$(git hash-object --no-filters "$fixture_dir/reject-double-await.atto")" == "bb866b2baf700a85e1c105b623c485aaa4189bbe" ]]
[[ ! -L "$fixture_dir/reject-const-taskfuture.atto" && "$(git hash-object --no-filters "$fixture_dir/reject-const-taskfuture.atto")" == "34eb2855945e9196f522fdb7e34ad609cd971d43" ]]
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
for opt in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  for name in taskfuture-int future-int; do
    src="$fixture_dir/$name.atto"
    fixture_sha="$(sha256sum "$src" | cut -d' ' -f1)"
    ir="$tmp/$name-O$opt.ll"
    out="$tmp/$name-O$opt"
    "$compiler" --target=host "$src" > "$ir"
    for symbol in oreslang_source_task_start_i64 oreslang_source_task_await_i64 oreslang_source_task_runtime_shutdown; do
      grep -Fq "@$symbol" "$ir"
    done
    "$auditor" "$ir" >/dev/null
    timeout 90s bash "$root/scripts/compile-aot.sh" "$src" "$out"
    [[ -x "$out" && ! -L "$out" ]]
    status=0
    timeout 15s "$out" >"$out.stdout" 2>"$out.stderr" || status=$?
    [[ "$status" == 42 && ! -s "$out.stdout" && ! -s "$out.stderr" ]]
    [[ "$(sha256sum "$src" | cut -d' ' -f1)" == "$fixture_sha" ]]
    [[ "$(sha256sum "$compiler" | cut -d' ' -f1)" == "$compiler_sha" ]]
    printf 'CLOSED_TASK_AOT_PASS|head=%s|case=%s|O%s|exit=42|compiler_sha256=%s|fixture_sha256=%s|artifact_sha256=%s\n' "$EXPECTED_HEAD" "$name" "$opt" "$compiler_sha" "$fixture_sha" "$(sha256sum "$out" | cut -d' ' -f1)"
  done
done
for name in reject-double-await reject-const-taskfuture taskfuture-string; do
  src="$fixture_dir/$name.atto"; out="$tmp/rejected-$name"
  status=0
  timeout 90s bash "$root/scripts/compile-aot.sh" "$src" "$out" >"$out.log" 2>&1 || status=$?
  [[ "$status" -gt 0 && "$status" -lt 124 && ! -e "$out" && ! -L "$out" ]]
  case "$name" in
    reject-double-await) grep -Fq 'E_TASK_RESULT_CONSUMED' "$out.log";;
    reject-const-taskfuture) grep -Fq 'E_CONST:' "$out.log";;
    taskfuture-string) grep -Fq "expected 'int'" "$out.log";;
  esac
  printf 'CLOSED_TASK_AOT_REJECT|head=%s|case=%s|no_artifact=yes\n' "$EXPECTED_HEAD" "$name"
done
[[ "$(sha256sum "$compiler" | cut -d' ' -f1)" == "$compiler_sha" ]]
[[ -z "$(git -C "$root" status --porcelain --untracked-files=no)" ]]
