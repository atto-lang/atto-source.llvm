#!/usr/bin/env bash
# Compile the admitted host-language subset to a standalone native executable.
# Unsupported source must fail in oreslang-llvmc, never fall back to an interpreter.
set -euo pipefail
[[ $# == 2 ]] || { echo 'Usage: scripts/compile-aot.sh source.ores output-executable' >&2; exit 64; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
clang="${CLANG:-clang}"
lto="${ORESLANG_LTO:-off}"
case "$lto" in off|full|thin) ;; *) echo 'ORESLANG_LTO must be off, full or thin.' >&2; exit 64 ;; esac
source_dir="$(cd "$(dirname "$1")" && pwd -P)"
source_file="$source_dir/$(basename "$1")"
output_dir="$(cd "$(dirname "$2")" && pwd -P)"
output_file="$output_dir/$(basename "$2")"
[[ ! -d "$output_file" ]] || { echo 'Output must be a file, not a directory.' >&2; exit 64; }
[[ "$source_file" != "$output_file" ]] || { echo 'Source and executable paths must differ.' >&2; exit 64; }
# String-path inequality is insufficient: an output hardlink or symlink can
# refer to the input inode, and atomic publication would destroy guest source.
[[ ! "$source_file" -ef "$output_file" ]] || {
  echo 'Source and executable must not refer to the same file.' >&2
  exit 64
}
[[ -x "$compiler" ]] || { echo 'Build oreslang-llvmc first or set ORESLANG_LLVMC.' >&2; exit 69; }
command -v "$clang" >/dev/null || { echo 'Install Clang or set CLANG.' >&2; exit 69; }
clang_binary="$(command -v "$clang")"
# An output hardlink, symlink or exact path to a toolchain executable would
# atomically replace the compiler or linker after successful compilation.
# Fail before invoking any tool and never publish over the toolchain.
for tool_file in "$compiler" "$clang_binary"; do
  [[ ! "$output_file" -ef "$tool_file" ]] || {
    echo 'Output must not alias the compiler or Clang executable.' >&2
    exit 64
  }
done
# An explicitly requested LTO mode must never silently degrade to normal -O2.
# The current source compiler produces one LLVM module; LTO here is a link-mode
# plumbing gate, NOT proof of cross-Oreslang-module optimization.
if [[ "$lto" != off ]]; then
  command -v ld.lld >/dev/null || { echo 'LTO requires ld.lld (LLVM LLD) on PATH.' >&2; exit 69; }
  [[ ! "$output_file" -ef "$(command -v ld.lld)" ]] || {
    echo 'Output must not alias the LLD linker executable.' >&2
    exit 64
  }
  "$clang" --version | grep -qi clang || { echo 'LTO requires Clang.' >&2; exit 69; }
fi
# Build beside the output so publication is atomic and errors preserve old output.
work="$(mktemp -d "$output_dir/.ores-aot.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$source_file" > "$work/program.ll"
if [[ "$lto" == off ]]; then
  "$clang" -O2 -x ir "$work/program.ll" -o "$work/program"
else
  "$clang" -O2 "-flto=$lto" -fuse-ld=lld -x ir "$work/program.ll" -o "$work/program"
fi
mv -f -- "$work/program" "$output_file"
printf 'Native executable: %s (LTO mode: %s)\n' "$output_file" "$lto"

