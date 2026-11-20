# Core-library source candidates (LLVM)

`assert.ores` is preserved, byte-for-byte, from the private LLVM #354
assert-import integration proposal. The source candidate uses the Oreslang
value-equality operators `eq`/`neq` and identity operator `is`.

**Status: source candidate only; not an installed native standard library.**
LLVM's current `Parser.cpp` parses a module import, but current
`emit_verified_llvm_ir` deliberately rejects every unresolved source import
with `native imports require filesystem module resolution`. Merely placing
`stdlib/assert.ores` in this tree must not grant an import capability.

The old #246/#354 scalar conformance corpus models `trap -> None`, whereas
modern native pure `trap` constructs a restricted `Result<int,Error>`.
The compiler and cross-backend specification must reconcile this effect
contract and prove typed `Any`, `bool`, cleanup, failure propagation, and
source-owned linking before activating `Assert`. No guest native execution
or Java parity is certified by this source file.
