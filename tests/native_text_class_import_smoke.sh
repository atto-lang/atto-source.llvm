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
# The import is checked against the opened inode, not a prior path lookup.
# A symlink supplied directly (or swapped in before open) must fail closed.
cp "$entry" "$work/main.ores"
rm -f "$work/main-1.ores"
ln -s "$(dirname "$entry")/main-1.ores" "$work/main-1.ores"
if "$compiler" "$work/main.ores" > "$work/invalid.ll" 2> "$work/error"; then
  echo "symlink class import unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/invalid.ll"
grep -q 'regular, non-symlink file' "$work/error"

# A swapped-in FIFO must neither be admitted nor hang the compiler at open().
rm -f "$work/main-1.ores"
mkfifo "$work/main-1.ores"
if "$compiler" "$work/main.ores" > "$work/invalid.ll" 2> "$work/error"; then
  echo "FIFO class import unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/invalid.ll"
grep -q 'regular, non-symlink file' "$work/error"
rm -f "$work/main-1.ores"

# No relative path traversal accepted by the bounded first-stage importer.
sed -e 's@"./main-1.ores"@"../main-1.ores"@' "$entry" > "$work/traversal.ores"
if "$compiler" "$work/traversal.ores" > "$work/traversal.ll" 2> "$work/error"; then
  echo "traversal unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/traversal.ll"
grep -q 'sibling .ores' "$work/error"

# Unrecognized, wrong-arity and recursive method calls must fail *before* IR.
cp "$(dirname "$entry")/main-1.ores" "$work/main-1.ores"
sed -e 's/f.bar()/f.missing()/' "$entry" > "$work/main.ores"
if "$compiler" "$work/main.ores" > "$work/invalid.ll" 2> "$work/error"; then
  echo "unknown method unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/invalid.ll"
grep -q 'unknown native class method' "$work/error"

sed -e 's/f.bar()/f.bar("unexpected")/' "$entry" > "$work/main.ores"
if "$compiler" "$work/main.ores" > "$work/invalid.ll" 2> "$work/error"; then
  echo "wrong method arity unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/invalid.ll"
grep -q "expected ')'" "$work/error"

cp "$entry" "$work/main.ores"
sed -e 's/return "hiiii";/return self.bar();/' \
    "$(dirname "$entry")/main-1.ores" > "$work/main-1.ores"
if "$compiler" "$work/main.ores" > "$work/invalid.ll" 2> "$work/error"; then
  echo "recursive native class method unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/invalid.ll"
grep -q 'recursive native string method cycle' "$work/error"

# Reject invalid byte sequences inside imported source, just like entry input.
cp "$(dirname "$entry")/main-1.ores" "$work/main-1.ores"
printf '\377' >> "$work/main-1.ores"
if "$compiler" "$work/main.ores" > "$work/invalid.ll" 2> "$work/error"; then
  echo "invalid imported UTF-8 unexpectedly accepted" >&2; exit 1
fi
test ! -s "$work/invalid.ll"
grep -q 'imported source is not valid UTF-8' "$work/error"
