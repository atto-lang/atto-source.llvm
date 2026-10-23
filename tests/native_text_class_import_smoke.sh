#!/usr/bin/env bash
# Executes compiled .ores source, not a C++ model or interpreter fallback.
set -euo pipefail
[[ $# == 3 ]] || { echo "usage: test <llvmc> <clang> <main.ores>" >&2; exit 64; }
compiler="$1"; clang="$2"; entry="$3"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
"$compiler" --target=host "$entry" > "$work/guest.ll"
"$clang" -O2 -Wno-override-module -x ir "$work/guest.ll" -o "$work/guest"
"$work/guest" > "$work/output"
printf 'hello world\nxhiiii 2hiiii\n' > "$work/expected"
diff -u "$work/expected" "$work/output"
# A private method must not be callable outside its declaring class.
cp "$entry" "$work/main.ores"
cp "$(dirname "$entry")/main-1.ores" "$work/main-1.ores"
sed -e 's/pub bar(): string/bar(): string/' "$work/main-1.ores" > "$work/library"
mv "$work/library" "$work/main-1.ores"
if "$compiler" "$work/main.ores" > "$work/invalid.ll" 2> "$work/error"; then
  echo "private method unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/invalid.ll"
grep -q 'method is private' "$work/error"
# No relative path traversal accepted by the bounded first-stage importer.
sed -e 's@"./main-1.ores"@"../main-1.ores"@' "$entry" > "$work/traversal.ores"
if "$compiler" "$work/traversal.ores" > "$work/traversal.ll" 2> "$work/error"; then
  echo "traversal unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/traversal.ll"
grep -q 'sibling .ores' "$work/error"
