#!/usr/bin/env bash
# Compile the admitted host-language subset to a standalone native executable.
# Unsupported source must fail in oreslang-llvmc, never fall back to an interpreter.
set -euo pipefail
[[ $# == 2 ]] || { echo 'Usage: scripts/compile-aot.sh source.atto|source.ores output-executable' >&2; exit 64; }
# Clang optimization level only; frontend semantics and the native no-GC IR
# admission gate must remain identical at both levels.
opt_level="${ORESLANG_LLVM_AOT_OPT_LEVEL:-2}"
case "$opt_level" in
  0|2) ;;
  *) echo 'ORESLANG_LLVM_AOT_OPT_LEVEL must be 0 or 2.' >&2; exit 64 ;;
esac
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
clang="${CLANG:-clang}"
source_dir="$(cd "$(dirname "$1")" && pwd -P)"
source_file="$source_dir/$(basename "$1")"
output_dir="$(cd "$(dirname "$2")" && pwd -P)"
output_file="$output_dir/$(basename "$2")"
[[ ! -d "$output_file" ]] || { echo 'Output must be a file, not a directory.' >&2; exit 64; }
[[ "$source_file" != "$output_file" ]] || { echo 'Source and executable paths must differ.' >&2; exit 64; }
[[ -x "$compiler" ]] || { echo 'Build oreslang-llvmc first or set ORESLANG_LLVMC.' >&2; exit 69; }
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
[[ -x "$auditor" ]] || { echo 'Build oreslang-no-gc-ir-audit first or set ORES_NO_GC_IR_AUDIT.' >&2; exit 69; }
clang_path="$(command -v "$clang")" || { echo 'Install Clang or set CLANG.' >&2; exit 69; }
# Compare identities, not just spellings: a symlink or hard link must not let
# publication replace the source compiler or native toolchain executable.
if [[ "$output_file" -ef "$source_file" || "$output_file" -ef "$compiler" ||
      "$output_file" -ef "$clang_path" || "$output_file" -ef "$auditor" ]]; then
  echo 'Output must not alias the source, Oreslang compiler, Clang or no-GC IR auditor.' >&2
  exit 64
fi
# Build beside the output so publication is atomic and errors preserve old output.
work="$(mktemp -d "$output_dir/.ores-aot.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$source_file" > "$work/program.ll"
# Known tracing-GC IR screen; not a complete linked-image/FFI collector proof.
"$auditor" "$work/program.ll" >/dev/null
runtime_sources=()
if grep -Eq 'declare .*@ores_string_' "$work/program.ll"; then
  runtime_sources+=("$root/src/main/cpp/oreslang/runtime/NativeStringRegion.c")
fi
if grep -Eq 'declare .*@ores_record_' "$work/program.ll"; then
  runtime_sources+=("$root/src/main/cpp/oreslang/runtime/NativeRecordRegion.c")
fi
if grep -Eq 'declare .*@oreslang_int_array_' "$work/program.ll"; then
  runtime_sources+=("$root/src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c")
fi
if grep -Eq 'declare .*@(oreslang_source_actor_|ores_actor_)' "$work/program.ll"; then
  # Source actors: the native scheduler, VM and actor-owned i64 heap. Same
  # set tests/source_actor_e2e.py links, minus its test-only receipt define.
  rt="$root/src/main/cpp/oreslang/runtime"
  runtime_sources+=(
    "$rt/ActorOwnedI64Heap.c" "$rt/NativeActorScheduler.c" "$rt/NativeOresVM.c"
    "$rt/NativeActorFuture.c" "$rt/SourceActorBootstrap.c"
    "$rt/NativeExecutionContextStore.c" "$rt/NativeContinuationStore.c"
    "$rt/NativeAwaitTraceBridge.c" "$rt/NativeLogicalTrace.c")
fi
# Native source Task<int> has a runtime ABI even when there is no actor import.
# Link it on the normal AOT path, not only the bespoke source Task test.
if grep -Eq 'declare .*@oreslang_source_task_(start_i64|await_i64|runtime_shutdown)' "$work/program.ll"; then
  rt="$root/src/main/cpp/oreslang/runtime"
  runtime_sources+=("$rt/SourceTaskRuntime.c")
  scheduler_source="$rt/NativeActorScheduler.c"
  scheduler_linked=0
  for runtime_source in "${runtime_sources[@]}"; do
    if [[ "$runtime_source" == "$scheduler_source" ]]; then
      scheduler_linked=1
      break
    fi
  done
  if [[ "$scheduler_linked" == 0 ]]; then
    runtime_sources+=("$scheduler_source")
  fi
fi
if (( ${#runtime_sources[@]} )); then
  "$clang" "-O${opt_level}" -x ir "$work/program.ll" -x c -std=c11 \
    -I "$root/src/main/cpp" "${runtime_sources[@]}" -pthread -o "$work/program"
else
  "$clang" "-O${opt_level}" -x ir "$work/program.ll" -o "$work/program"
fi
mv -f -- "$work/program" "$output_file"
printf 'Native executable: %s\n' "$output_file"

