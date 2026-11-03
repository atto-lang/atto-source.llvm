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
    # Also reject a shared library or forged header with no executable entry
    # or valid ELF64 program-header table. Only explicitly qualified little-
    # endian ELF64 host targets are supported by this driver.
    elf_ident_version="$(LC_ALL=C od -An -tu1 -j6 -N1 "$work/program" | tr -d '[:space:]')"
    elf_version="$(LC_ALL=C od -An -tu4 -j20 -N4 "$work/program" | tr -d '[:space:]')"
    elf_entry="$(LC_ALL=C od -An -tu8 -j24 -N8 "$work/program" | tr -d '[:space:]')"
    elf_phoff="$(LC_ALL=C od -An -tu8 -j32 -N8 "$work/program" | tr -d '[:space:]')"
    elf_ehsize="$(LC_ALL=C od -An -tu2 -j52 -N2 "$work/program" | tr -d '[:space:]')"
    elf_phentsize="$(LC_ALL=C od -An -tu2 -j54 -N2 "$work/program" | tr -d '[:space:]')"
    elf_phnum="$(LC_ALL=C od -An -tu2 -j56 -N2 "$work/program" | tr -d '[:space:]')"
    image_bytes="$(wc -c < "$work/program" | tr -d '[:space:]')"
    [[ "$elf_class" == 2 && "$elf_data" == 1 &&
       "$elf_ident_version" == 1 && "$elf_version" == 1 &&
       ( "$elf_kind" == 2 || "$elf_kind" == 3 ) &&
       "$elf_machine" == "$host_machine" &&
       "$elf_entry" =~ ^[0-9]+$ && "$elf_entry" != 0 &&
       "$elf_ehsize" == 64 && "$elf_phentsize" == 56 &&
       "$elf_phnum" =~ ^[0-9]+$ &&
       "$elf_phoff" =~ ^[0-9]+$ ]] || {
      echo "ELF architecture, version, entrypoint or program-header mismatch" >&2
      exit 65
    }
    # Check bounds before addition/multiplication to avoid overflow on a
    # corrupt 64-bit header. The file-size bound is much smaller than u64.
    if (( elf_phnum == 0 || elf_phoff < 64 || elf_phoff > image_bytes ||
          elf_phnum > image_bytes / elf_phentsize ||
          elf_phoff > image_bytes - elf_phnum * elf_phentsize )); then
      echo 'ELF program-header table is missing or outside the artifact.' >&2
      exit 65
    fi
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
