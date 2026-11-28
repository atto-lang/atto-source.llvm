#!/usr/bin/env bash
# Internal AOT driver helper. Input IR has already passed compiler/IR admission.
# All paths are inside the driver's private staging directory. This is not
# namespace linking, a persistent cache or a verifier for arbitrary objects.
set -euo pipefail
[[ $# == 5 ]] || { echo 'usage: link-scalar-object.sh CLANG OPT IR OBJECT EXECUTABLE' >&2; exit 64; }
clang="$1"; opt="$2"; ir="$3"; object="$4"; executable="$5"
[[ "$(uname -s)" == Linux && ( "$opt" == 0 || "$opt" == 2 ) ]] || exit 64
[[ -f "$ir" && ! -L "$ir" && ! -e "$object" && ! -L "$object" && ! -e "$executable" && ! -L "$executable" && "$object" != "$executable" ]] || exit 64
"$clang" "-O$opt" -x ir -c "$ir" -o "$object"
[[ -s "$object" && -f "$object" && ! -L "$object" ]] || exit 65
# A true relocatable ELF object, not textual IR, bitcode or executable.
header="$(LC_ALL=C readelf -hW "$object" 2>&1)" || exit 65
grep -Eq '^[[:space:]]*Type:[[:space:]]+REL[[:space:]]' <<<"$header" || exit 65
if grep -Eq '(^|[[:space:]])(Error|Warning):' <<<"$header"; then exit 65; fi
"$clang" "-O$opt" "$object" -o "$executable"
# The caller still runs its full linked-image gate before publishing.
[[ -s "$executable" && -x "$executable" && ! -L "$executable" ]] || exit 65
