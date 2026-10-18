#!/usr/bin/env bash
# Compile the admitted host-language subset to a standalone native executable.
# Unsupported source must fail in oreslang-llvmc, never fall back to an interpreter.
set -euo pipefail
[[ $# == 2 ]] || { echo 'Usage: scripts/compile-aot.sh source.ores output-executable' >&2; exit 64; }
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
output_dir="$(cd "$(dirname "$2")" && pwd -P)"
output_file="$output_dir/$(basename "$2")"
[[ ! -d "$output_file" ]] || { echo 'Output must be a file, not a directory.' >&2; exit 64; }
[[ "$source_file" != "$output_file" ]] || { echo 'Source and executable paths must differ.' >&2; exit 64; }
[[ -x "$compiler" ]] || { echo 'Build oreslang-llvmc first or set ORESLANG_LLVMC.' >&2; exit 69; }
command -v "$clang" >/dev/null || { echo 'Install Clang or set CLANG.' >&2; exit 69; }
# Build beside the output so publication is atomic and errors preserve old output.
work="$(mktemp -d "$output_dir/.ores-aot.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
# Probe the actual compiler+linker combination before touching the published output.
# A missing LLD plugin or incompatible LLVM bitcode must be a hard error, never
# an implicit fallback to a non-LTO link.
if [[ "$lto_mode" != off ]]; then
  if ! printf 'int main(void){return 0;}\n' |
    "$clang" -O2 "${lto_flags[@]}" -x c - -o "$work/lto-probe" >"$work/lto-probe.log" 2>&1; then
    cat "$work/lto-probe.log" >&2
    echo "Requested $lto_mode LTO is unsupported by CLANG/linker; aborting." >&2
    exit 69
  fi
fi
"$compiler" --target=host "$source_file" > "$work/program.ll"
"$clang" -O2 "${lto_flags[@]}" -x ir "$work/program.ll" -o "$work/program"
mv -f -- "$work/program" "$output_file"
printf 'Native executable: %s\n' "$output_file"
