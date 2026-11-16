#!/usr/bin/env bash
# Real .atto -> oreslang-llvmc --target=wasm32 -> LLVM IR -> Clang wasm32
# -> import-free WebAssembly main:()->i64. Not an actor or inverted-class ABI.
set -euo pipefail
compiler="$1"
clang="$2"
fixtures="$3"
test -x "$compiler" && test -f "$compiler" && test ! -L "$compiler"
test -d "$fixtures"
command -v node >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for name in swap-reuse full-width-i64 removed-index-trap; do
  "$compiler" --target=wasm32 "$fixtures/$name.atto" >"$tmp/$name.ll" 2>"$tmp/$name.compile.err"
  test -s "$tmp/$name.ll"
  test "$(grep -Ec '^target triple = "wasm32-unknown-unknown"$' "$tmp/$name.ll")" = 1
  grep -Fq 'define i64 @main()' "$tmp/$name.ll"
  grep -Fq 'declare void @llvm.trap()' "$tmp/$name.ll"
  grep -Fq 'store volatile i1 false, ptr %alive' "$tmp/$name.ll"
  if grep -Eq '(^|[^a-zA-Z_])(GC_malloc|GC_collect|Boehm|GC_init|malloc|free)[( ]' "$tmp/$name.ll"; then
    echo "unapproved managed allocation in $name IR" >&2; exit 1
  fi
  if grep -Fq 'trunc i64' "$tmp/$name.ll"; then
    echo "Wasm ABI truncated i64 in $name IR" >&2; exit 1
  fi
  for opt in 0 2; do
    "$clang" --target=wasm32 -x ir -O"$opt" -nostdlib \
      -Wl,--no-entry -Wl,--export=main -Wl,--export-memory \
      -Wl,--initial-memory=131072 -Wl,--max-memory=131072 \
      -o "$tmp/$name-$opt.wasm" "$tmp/$name.ll"
    node --input-type=module - "$tmp/$name-$opt.wasm" "$name" <<'NODE'
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
const [file,name]=process.argv.slice(2);
const binary=readFileSync(file);
assert.ok(WebAssembly.validate(binary),"real compiled guest must be valid Wasm");
const module=new WebAssembly.Module(binary);
assert.deepEqual(WebAssembly.Module.imports(module),[],"forbid every host import");
assert.deepEqual(WebAssembly.Module.exports(module)
 .map(({name,kind})=>name+":"+kind).sort(),
 ["main:function","memory:memory"],"closed guest exports");
const guest=new WebAssembly.Instance(module);
assert.equal(guest.exports.memory.buffer.byteLength,131072,"bounded private memory");
assert.equal(typeof guest.exports.main,"function");
if(name==="removed-index-trap") {
 assert.throws(()=>guest.exports.main(),WebAssembly.RuntimeError,
  "removed dense index must trap, never return stale data");
} else {
 const expected=name==="full-width-i64"?4294967338n:42n;
 assert.equal(guest.exports.main(),expected,"full-width source guest result");
}
NODE
    echo "ATTO_INVERTED_WASM32_SOURCE_GUEST_${name}_O${opt}_EXECUTED"
  done
done

# Source-level negative cases must be compiler-owned rejections, with no IR.
cat >"$tmp/unsupported-class.atto" <<'ATTO'
define inverted class Row as
    let value: int;
end
fnc main(): int { return 42; }
ATTO
if "$compiler" --target=wasm32 "$tmp/unsupported-class.atto" >"$tmp/rejected.ll" 2>"$tmp/rejected.err"; then
  echo 'inverted class incorrectly admitted' >&2; exit 1
fi
test ! -s "$tmp/rejected.ll"
grep -Fq 'DOP_INVERTED_CLASS_UNSUPPORTED' "$tmp/rejected.err"

echo 'REAL_ATTO_INVERTED_WASM32_SOURCE_GUEST_PASS; two_i64_stack_local_only; O0+O2; full_i64; bounds_trap'
