#!/usr/bin/env bash
# Restricted host-only module provider object. Trusted compiler/toolchain and
# trusted output parent required. Physical i64() ABI only, not a language
# namespace interface or authenticated package/cache receipt.
set -euo pipefail
fail(){ echo "E_MODULE_OBJECT: $*" >&2; exit 65; }
[[ $# == 4 ]] || { echo 'usage: compile-scalar-module-object.sh SOURCE OUTPUT MODULE FUNCTION' >&2; exit 64; }
[[ "$(uname -s)" == Linux ]] || fail 'Linux ELF only'
[[ "$3" =~ ^[A-Za-z_][A-Za-z_0-9]{0,63}$ && "$4" =~ ^[A-Za-z_][A-Za-z_0-9]{0,63}$ && "$4" != main ]] || fail 'invalid module or export'
symbol="$3.$4"
opt="${ORESLANG_LLVM_AOT_OPT_LEVEL:-2}"
[[ "$opt" == 0 || "$opt" == 2 ]] || fail 'optimization must be 0 or 2'
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
clang="$(command -v "${CLANG:-clang}")" || fail 'Clang required'
[[ -x "$compiler" && -x "$auditor" ]] || fail 'compiler and auditor required'
source="$(realpath -e -- "$1")" || fail 'missing source'
[[ -f "$source" ]] || fail 'regular source required'
parent="$(cd "$(dirname "$2")" && pwd -P)"
output="$parent/$(basename "$2")"
admit_destination(){
  [[ ! -L "$output" && ( ! -e "$output" || -f "$output" ) ]] || fail 'destination must be absent or a regular non-symlink'
  [[ "$output" != "$source" && ! "$output" -ef "$source" && ! "$output" -ef "$compiler" && ! "$output" -ef "$auditor" && ! "$output" -ef "$clang" ]] || fail 'protected input alias'
}
admit_destination
work="$(mktemp -d "$parent/.atto-module-object.XXXXXX")"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$source" >"$work/provider.ll"
"$auditor" "$work/provider.ll" >/dev/null
# The compiler emits canonical LLVM IR. Restrict this first adapter to a
# single physical i64() definition, no other function bodies or globals.
# Intrinsic declarations are allowed; external object relocations are not.
awk -v symbol="$symbol" '
  /^define[[:space:]]/ {
    definitions++
    if (index($0, "define i64 @" symbol "()") == 1 ||
        index($0, "define i64 @\"" symbol "\"()") == 1) expected++
  }
  /^@/ { globals++ }
  END { exit !(definitions==1 && expected==1 && globals==0) }
' "$work/provider.ll" || fail 'expected one i64() module export and no global definitions'
"$clang" "-O$opt" -x ir -c "$work/provider.ll" -o "$work/provider.o"
LC_ALL=C readelf -hW "$work/provider.o" >"$work/header"
grep -Eq '^[[:space:]]*Type:[[:space:]]+REL[[:space:]]' "$work/header" || fail 'not relocatable ELF'
LC_ALL=C nm --defined-only --extern-only --format=posix "$work/provider.o" >"$work/defined"
awk -v symbol="$symbol" '$1==symbol && $2=="T" { n++ } END { exit !(NR==1 && n==1) }' "$work/defined" || fail 'unexpected defined external symbol inventory'
LC_ALL=C nm --undefined-only "$work/provider.o" >"$work/undefined"
[[ ! -s "$work/undefined" ]] || fail 'unresolved external symbols'
admit_destination
mv -T -- "$work/provider.o" "$output"
printf 'MODULE_OBJECT|symbol=%s|O%s|sha256=%s\n' "$symbol" "$opt" "$(sha256sum "$output" | cut -d' ' -f1)"
