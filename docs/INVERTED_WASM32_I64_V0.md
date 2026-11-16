# Verified inverted i64 pair → wasm32 (v0 draft)

This compiler already admits one restricted production `.atto` source shape:
`define inverted struct Row` with exactly two i64 columns, a local
`InvertedArray<Row>` owner (capacity 1..256), checked push/field-update/
swap-delete/return-read, and a terminal MIR close. The native host target
retains its original `define i32 @main()` process-exit ABI.

**This draft extends that same verified HIR/MIR path to
`oreslang-llvmc --target=wasm32`, emitting
`target triple = "wasm32-unknown-unknown"` and
`define i64 @main()` with NO i32 truncation.** The LLVM emitter retains
range checks, `llvm.trap` on invalid dense indexes, owner liveness checks,
and volatile scrubbing of both columns, generation metadata and logical
length before returning the detached scalar. This is stack-local SoA only;
there is no actor-owned heap or stable external row handle in this slice.

Execute full-tree tests with an actual exact-head compiler:

```sh
bash tests/native_inverted_mir_codegen_smoke.sh
bash tests/native_inverted_source_guest_hardening.sh /absolute/oreslang-llvmc clang-18 contracts/dop-inverted-v1
COMPILER_PIN="$(sha256sum /absolute/oreslang-llvmc | cut -d ' ' -f 1)"
bash tests/native_inverted_wasm32_source_guest.sh /absolute/oreslang-llvmc clang-18 contracts/dop-inverted-v1 "$COMPILER_PIN"
```

The wasm32 test compiles three real `.atto` files at `-O0` and `-O2`
through Clang/LLD, validates zero Wasm imports, exact `main` and memory
exports, fixed 2-page memory and full i64 values in a fresh guest. It proves
42 from dense swap-delete and reuse, returns 4294967338 without native i32
truncation, and traps on an invalid erased index. It rejects an inverted
class at the source compiler boundary without partial stdout IR.

The dedicated exact-PR-head workflow hashes the built compiler and passes the digest
as an explicit byte-identity pin to the smoke harness, checking it before and
after execution. The SHA alone is not independent source-origin approval.
The workflow appends this gate to existing native
LLVM tests. CI with no runner job steps, missing toolchain, or no actual
LLVM compiler remains **NOT_EXECUTED**. This does not prove browser-host
isolation, native serialized Wasmtime AOT, LLVM/Java/.NET actor parity,
zero GC in the host, or hard real-time behavior.
