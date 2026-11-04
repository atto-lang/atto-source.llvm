#!/usr/bin/env bash
# Compile real .atto fuzz_byte(i64) guest plus bounded C stdin transport.
# Not a general AttoLang stdin API; the host adapter checks the guest property.
set -euo pipefail
if (( $# != 2 )); then
  echo 'usage: build-native-atto-fuzz-target.sh source.atto output-executable' >&2
  exit 64
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
clang="${CLANG:-clang}"
sanitizers="${ATTO_FUZZ_SANITIZERS:-none}"
case "$sanitizers" in
  none|address,undefined) ;;
  *) echo 'unsupported ATTO_FUZZ_SANITIZERS; use none or address,undefined' >&2; exit 64;;
esac
case "$1" in
  *.atto|*.ores) ;;
  *) echo 'native fuzz guest must use canonical .atto or legacy .ores extension' >&2; exit 64;;
esac
source_dir="$(cd "$(dirname "$1")" && pwd -P)"
source_file="$source_dir/$(basename "$1")"
output_dir="$(cd "$(dirname "$2")" && pwd -P)"
output_file="$output_dir/$(basename "$2")"
adapter="$root/tests/native_atto_fuzz_stdin_adapter.c"
[[ -f "$source_file" && ! -d "$output_file" && ! -L "$output_file" ]] ||
  { echo 'invalid source or output path' >&2; exit 64; }
[[ -x "$compiler" && -x "$auditor" && -f "$adapter" ]] ||
  { echo 'missing executable LLVM compiler or Rust IR auditor or stdin adapter' >&2; exit 69; }
clang_path="$(command -v "$clang")" ||
  { echo 'Clang not found' >&2; exit 69; }
for protected in "$source_file" "$compiler" "$auditor" "$clang_path" "$adapter"; do
  if [[ "$output_file" == "$protected" || "$output_file" -ef "$protected" ]]; then
    echo 'output aliases an input or toolchain executable' >&2
    exit 64
  fi
done
work="$(mktemp -d "$output_dir/.atto-fuzz-build.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$source_file" > "$work/guest.ll"
[[ -s "$work/guest.ll" ]] ||
  { echo 'LLVM compiler emitted empty IR' >&2; exit 1; }
bytes="$(wc -c < "$work/guest.ll")"
(( bytes <= 2000000 )) ||
  { echo 'LLVM guest IR exceeds bounded size' >&2; exit 1; }
"$auditor" "$work/guest.ll" >/dev/null
count="$(grep -Ec '^define (dso_local )?i64 @fuzz_byte\(i64[[:space:]][^,)]*\)[[:space:]]' "$work/guest.ll" || true)"
[[ "$count" == 1 ]] ||
  { echo 'LLVM guest must define exactly one external fuzz_byte(i64) -> i64' >&2; exit 1; }
if grep -Eq '^define .*@main\(' "$work/guest.ll"; then
  echo 'LLVM fuzz guest must not provide a native main; the host adapter owns stdin' >&2
  exit 1
fi
flags=(-O1 -g -fno-omit-frame-pointer -Wno-override-module)
if [[ "$sanitizers" == address,undefined ]]; then
  flags+=(-fsanitize=address,undefined -fno-sanitize-recover=all)
fi
"$clang" "${flags[@]}" -x ir "$work/guest.ll" -x c -std=c11 \
  -Wall -Wextra -Wpedantic -Werror "$adapter" -o "$work/target"
[[ -f "$work/target" && -x "$work/target" ]] ||
  { echo 'native fuzz target did not link to an executable' >&2; exit 1; }
mv -f -- "$work/target" "$output_file"
printf 'ATTO_LLVM_FUZZ_TARGET_BUILT=%s sanitize=%s\n' "$output_file" "$sanitizers"
