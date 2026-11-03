#!/usr/bin/env bash
# Compile the admitted host-language subset to a standalone native executable.
# Unsupported source must fail in oreslang-llvmc, never fall back to an interpreter.
set -euo pipefail
[[ $# == 2 ]] || { echo 'Usage: scripts/compile-aot.sh source.ores output-executable' >&2; exit 64; }
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
clang_path="$(command -v "$clang")" || { echo 'Install Clang or set CLANG.' >&2; exit 69; }
# Compare identities, not just spellings: a symlink or hard link must not let
# publication replace the source compiler or native toolchain executable.
if [[ "$output_file" -ef "$source_file" || "$output_file" -ef "$compiler" || "$output_file" -ef "$clang_path" ]]; then
  echo 'Output must not alias the source, Oreslang compiler or Clang.' >&2
  exit 64
fi
# Build beside the output so publication is atomic and errors preserve old output.
work="$(mktemp -d "$output_dir/.ores-aot.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$source_file" > "$work/program.ll"
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
if (( ${#runtime_sources[@]} )); then
  "$clang" -O2 -x ir "$work/program.ll" -x c -std=c11 \
    -I "$root/src/main/cpp" "${runtime_sources[@]}" -pthread -o "$work/program"
else
  "$clang" -O2 -x ir "$work/program.ll" -o "$work/program"
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
       ( "$elf_kind" == 2 || "$elf_kind" == 3 ) &&
       "$elf_machine" == "$host_machine" ]] || {
      echo "ELF architecture or executable type mismatch: class=$elf_class data=$elf_data kind=$elf_kind machine=$elf_machine" >&2
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
    [[ "$macho_kind" == 2 && "$macho_cpu" == "$host_cpu" ]] || {
      echo "Mach-O architecture or executable type mismatch: cpu=$macho_cpu kind=$macho_kind" >&2
      exit 65
    }
    ;;
  *)
    echo 'Standalone AOT driver has not been qualified on this host OS.' >&2
    exit 69
    ;;
esac
mv -f -- "$work/program" "$output_file"
printf 'Native executable: %s\n' "$output_file"
