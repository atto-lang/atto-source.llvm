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
# Strict opt-in: this only optimizes the currently admitted *single source* IR.
# Cross-Oreslang-module LTO requires a future multi-unit compiler driver.
lto_mode="${ORESLANG_AOT_LTO:-off}"
lto_flags=()
case "$lto_mode" in
  off) ;;
  full) lto_flags=(-flto -fuse-ld=lld) ;;
  thin) lto_flags=(-flto=thin -fuse-ld=lld) ;;
  *) echo 'ORESLANG_AOT_LTO must be off, full or thin.' >&2; exit 64 ;;
esac
source_dir="$(cd "$(dirname "$1")" && pwd -P)"
source_file="$source_dir/$(basename "$1")"
# Optional fail-closed scalar-only closed-world check; legacy AOT behavior
# remains unchanged unless explicitly requested by the caller.
aot_admission="${ORESLANG_LLVM_AOT_PROFILE:-off}"
compiler_aot_args=()
case "$aot_admission" in
  off) ;;
  sealed-scalar-v1) compiler_aot_args=(--aot-profile=sealed-scalar-v1) ;;
  sealed-class-fields-v1) compiler_aot_args=(--aot-profile=sealed-class-fields-v1) ;;
  actor-region-v1) compiler_aot_args=(--aot-profile=actor-region-v1) ;;
  *) echo 'ORESLANG_LLVM_AOT_PROFILE must be off, sealed-scalar-v1, sealed-class-fields-v1 or actor-region-v1.' >&2; exit 64 ;;
esac
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
# Check real cross-module linking, not just acceptance of an LTO flag.
# This synthetic LLVM IR canary is NOT a claim of Oreslang module support.
if [[ "$lto_mode" != off ]]; then
  cat > "$work/lto-helper.ll" <<'ORES_LTO_HELPER'
define i32 @ores_lto_helper(i32 %x) {
entry:
  %r = add i32 %x, 1
  ret i32 %r
}
ORES_LTO_HELPER
  cat > "$work/lto-main.ll" <<'ORES_LTO_MAIN'
declare i32 @ores_lto_helper(i32)
define i32 @main() {
entry:
  %result = call i32 @ores_lto_helper(i32 41)
  %is_correct = icmp eq i32 %result, 42
  %status = select i1 %is_correct, i32 0, i32 1
  ret i32 %status
}
ORES_LTO_MAIN
  if ! "$clang" -O2 "${lto_flags[@]}" -x ir "$work/lto-helper.ll" "$work/lto-main.ll" -o "$work/lto-probe" >"$work/lto-probe.log" 2>&1; then
    cat "$work/lto-probe.log" >&2
    echo "Requested $lto_mode LTO cannot link two LLVM IR units with this toolchain." >&2
    exit 69
  fi
  if ! "$work/lto-probe" >"$work/lto-execution.log" 2>&1; then
    cat "$work/lto-execution.log" >&2
    echo "Requested $lto_mode LTO canary did not execute correctly on the host." >&2
    exit 69
  fi
fi
"$compiler" ${compiler_aot_args[@]+"${compiler_aot_args[@]}"} --target=host "$source_file" > "$work/program.ll"
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
if grep -Eq 'declare .*@ores_string_split_literal' "$work/program.ll"; then
  # Compiler-generated String.split_literal owns a checked Array<String>.
  # Keep the cross-region helper separate from the basic String runtime
  # so existing String-only and Array-only link targets remain independent.
  runtime_sources+=("$root/src/main/cpp/oreslang/runtime/NativeStringSplit.c")
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
# ActorRegionGuestV1 currently has NO certified sealed-AOT linker closure.
# Discover the exact direct C source dependencies so the fail-closed gate below
# rejects before Clang or output publication; never silently link guest imports.
if grep -Eq 'declare .*@ores_actor_region_guest_v1_' "$work/program.ll"; then
  rt="$root/src/main/cpp/oreslang/runtime"
  runtime_sources+=("$rt/ActorRegionGuestV1.c" "$rt/ActorRegionV1.c" "$rt/ActorArena.c")
fi
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
# Full guest AOT + zero-tracing-GC requires a closed *linked* runtime graph,
# not merely an LLVM IR screen. Preserve the existing string, record, array,
# actor and Task runtime code for the ordinary AOT profile; none is yet
# certified as a transitive bounded, deterministic zero-GC sealed dependency.
# Each native runtime must eventually bring source ownership/drop proofs,
# actor cancellation/restart quiescence, an FFI symbol audit and guest receipts.
# Reject after discovering runtime imports but BEFORE invoking Clang or
# publishing an output. Never treat future native services as implicitly safe.
if [[ "$aot_admission" != off ]] && (( ${#runtime_sources[@]} > 0 )); then
  echo 'sealed AOT refuses unchecked native runtime dependency; require a certified zero-GC link closure' >&2
  exit 65
fi
if (( ${#runtime_sources[@]} )); then
  "$clang" "-O${opt_level}" -x ir "$work/program.ll" -x c -std=c11 \
    -I "$root/src/main/cpp" "${runtime_sources[@]}" -pthread -o "$work/program"
else
  "$clang" "-O${opt_level}" -x ir "$work/program.ll" -o "$work/program"
fi
# Fail closed before publishing. A successful subprocess exit is not proof that
# the output is a native executable: a broken wrapper can emit IR, a shell script,
# a library or nothing. The native ABI is verified again by the Rust CTest.
[[ -s "$work/program" && -x "$work/program" ]] || {
  echo 'Clang did not produce a nonempty executable.' >&2
  exit 65
}
magic="$(LC_ALL=C od -An -tx1 -N4 "$work/program" | tr -d '[:space:]')"
case "$(uname -s)" in
  Linux*)
    [[ "$magic" == 7f454c46 ]] || {
      echo "Expected ELF native executable, found magic $magic" >&2
      exit 65
    }
    elf_class="$(LC_ALL=C od -An -tu1 -j4 -N1 "$work/program" | tr -d '[:space:]')"
    elf_data="$(LC_ALL=C od -An -tu1 -j5 -N1 "$work/program" | tr -d '[:space:]')"
    elf_kind="$(LC_ALL=C od -An -tu2 -j16 -N2 "$work/program" | tr -d '[:space:]')"
    elf_machine="$(LC_ALL=C od -An -tu2 -j18 -N2 "$work/program" | tr -d '[:space:]')"
    case "$(uname -m)" in
      x86_64) host_machine=62 ;;
      aarch64|arm64) host_machine=183 ;;
      *) echo 'Unqualified Linux native AOT architecture.' >&2; exit 69 ;;
    esac
    [[ "$elf_class" == 2 && "$elf_data" == 1 &&
       ( "$elf_kind" == 2 || "$elf_kind" == 3 ) ]] || {
      echo "Wrong ELF class, endian or type: $elf_class $elf_data $elf_kind" >&2
      exit 65
    }
    [[ "$elf_machine" == "$host_machine" ]] || {
      echo "ELF architecture mismatch: $elf_machine != $host_machine" >&2
      exit 65
    }
    # ELF magic/machine alone can be forged. Require a real entrypoint and a
    # bounded, executable PT_LOAD program header before atomic publication.
    # Current release profile supports ordinary ELF64 files (<4 GiB); reject
    # extended numbering or 64-bit table offsets until separately qualified.
    elf_size="$(wc -c < "$work/program" | tr -d '[:space:]')"
    [[ "$elf_size" -ge 120 && "$elf_size" -lt 4294967296 ]] || {
      echo 'ELF image is too short or exceeds the bounded release profile.' >&2
      exit 65
    }
    elf_ident_version="$(LC_ALL=C od -An -tu1 -j6 -N1 "$work/program" | tr -d '[:space:]')"
    elf_version="$(LC_ALL=C od -An -tu4 -j20 -N4 "$work/program" | tr -d '[:space:]')"
    elf_entry_lo="$(LC_ALL=C od -An -tu4 -j24 -N4 "$work/program" | tr -d '[:space:]')"
    elf_entry_hi="$(LC_ALL=C od -An -tu4 -j28 -N4 "$work/program" | tr -d '[:space:]')"
    phoff="$(LC_ALL=C od -An -tu4 -j32 -N4 "$work/program" | tr -d '[:space:]')"
    phoff_hi="$(LC_ALL=C od -An -tu4 -j36 -N4 "$work/program" | tr -d '[:space:]')"
    ehsize="$(LC_ALL=C od -An -tu2 -j52 -N2 "$work/program" | tr -d '[:space:]')"
    phentsize="$(LC_ALL=C od -An -tu2 -j54 -N2 "$work/program" | tr -d '[:space:]')"
    phnum="$(LC_ALL=C od -An -tu2 -j56 -N2 "$work/program" | tr -d '[:space:]')"
    [[ "$elf_ident_version" == 1 && "$elf_version" == 1 &&
       ( "$elf_entry_lo" != 0 || "$elf_entry_hi" != 0 ) &&
       "$phoff_hi" == 0 && "$ehsize" == 64 && "$phentsize" == 56 &&
       "$phnum" -gt 0 && "$phnum" -le 1024 && "$phoff" -ge 64 &&
       $((phoff + phnum * phentsize)) -le "$elf_size" ]] || {
      echo 'ELF entrypoint or program-header table is missing/invalid.' >&2
      exit 65
    }
    entry_mapped=0
    for ((index=0; index<phnum; index++)); do
      header=$((phoff + index * phentsize))
      ptype="$(LC_ALL=C od -An -tu4 -j"$header" -N4 "$work/program" | tr -d '[:space:]')"
      [[ "$ptype" == 1 ]] || continue
      pflags="$(LC_ALL=C od -An -tu4 -j"$((header + 4))" -N4 "$work/program" | tr -d '[:space:]')"
      poffset="$(LC_ALL=C od -An -tu4 -j"$((header + 8))" -N4 "$work/program" | tr -d '[:space:]')"
      poffset_hi="$(LC_ALL=C od -An -tu4 -j"$((header + 12))" -N4 "$work/program" | tr -d '[:space:]')"
      pvaddr_lo="$(LC_ALL=C od -An -tu4 -j"$((header + 16))" -N4 "$work/program" | tr -d '[:space:]')"
      pvaddr_hi="$(LC_ALL=C od -An -tu4 -j"$((header + 20))" -N4 "$work/program" | tr -d '[:space:]')"
      pfilesz="$(LC_ALL=C od -An -tu4 -j"$((header + 32))" -N4 "$work/program" | tr -d '[:space:]')"
      pfilesz_hi="$(LC_ALL=C od -An -tu4 -j"$((header + 36))" -N4 "$work/program" | tr -d '[:space:]')"
      pmemsz_lo="$(LC_ALL=C od -An -tu4 -j"$((header + 40))" -N4 "$work/program" | tr -d '[:space:]')"
      pmemsz_hi="$(LC_ALL=C od -An -tu4 -j"$((header + 44))" -N4 "$work/program" | tr -d '[:space:]')"
      [[ "$poffset_hi" == 0 && "$pfilesz_hi" == 0 &&
         "$poffset" -le "$elf_size" &&
         "$pfilesz" -le $((elf_size - poffset)) &&
         ( "$pmemsz_hi" -gt 0 || "$pmemsz_lo" -ge "$pfilesz" ) ]] || {
        echo 'ELF PT_LOAD segment has invalid file bounds or filesz > memsz.' >&2
        exit 65
      }
      (( (pflags & 1) != 0 && pfilesz > 0 )) || continue
      # Compare the 64-bit entrypoint and vaddr in unsigned 32-bit limbs.
      # The file-backed segment is <4GiB in the bounded release profile;
      # allow crossing one limb boundary but reject unrelated entrypoints.
      if (( elf_entry_hi == pvaddr_hi && elf_entry_lo >= pvaddr_lo )); then
        delta=$((elf_entry_lo - pvaddr_lo))
      elif (( pvaddr_hi < 4294967295 && elf_entry_hi == pvaddr_hi + 1 &&
               elf_entry_lo < pvaddr_lo )); then
        delta=$((4294967296 - pvaddr_lo + elf_entry_lo))
      else
        continue
      fi
      if (( delta < pfilesz )); then
        entry_mapped=1
      fi
    done
    [[ "$entry_mapped" == 1 ]] || {
      echo 'ELF entrypoint is not mapped by a file-backed executable PT_LOAD.' >&2
      exit 65
    }
    ;;
  Darwin*)
    [[ "$magic" == cffaedfe ]] || {
      echo "Expected little-endian Mach-O 64-bit executable, found magic $magic" >&2
      exit 65
    }
    macho_cpu="$(LC_ALL=C od -An -tu4 -j4 -N4 "$work/program" | tr -d '[:space:]')"
    macho_kind="$(LC_ALL=C od -An -tu4 -j12 -N4 "$work/program" | tr -d '[:space:]')"
    case "$(uname -m)" in
      x86_64) host_cpu=16777223 ;;
      arm64|aarch64) host_cpu=16777228 ;;
      *) echo 'Unqualified macOS native AOT architecture.' >&2; exit 69 ;;
    esac
    macho_ncmds="$(LC_ALL=C od -An -tu4 -j16 -N4 "$work/program" | tr -d '[:space:]')"
    macho_sizeofcmds="$(LC_ALL=C od -An -tu4 -j20 -N4 "$work/program" | tr -d '[:space:]')"
    image_bytes="$(wc -c < "$work/program" | tr -d '[:space:]')"
    [[ "$macho_kind" == 2 && "$macho_cpu" == "$host_cpu" &&
       "$macho_ncmds" =~ ^[0-9]+$ && "$macho_sizeofcmds" =~ ^[0-9]+$ ]] || {
      echo "Mach-O architecture, executable type or command header mismatch" >&2
      exit 65
    }
    if (( macho_ncmds == 0 || macho_sizeofcmds < macho_ncmds * 8 ||
          image_bytes < 32 || macho_sizeofcmds > image_bytes - 32 )); then
      echo 'Mach-O has no valid bounded load-command table.' >&2
      exit 65
    fi
    ;;
  *)
    echo 'Standalone AOT driver has not been qualified on this host OS.' >&2
    exit 69
    ;;
esac
mv -f -- "$work/program" "$output_file"
printf 'Native executable: %s\n' "$output_file"

