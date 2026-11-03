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
    ;;
  Darwin*)
    [[ "$magic" == cffaedfe || "$magic" == feedfacf ]] || {
      echo "Expected Mach-O native executable, found magic $magic" >&2
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
