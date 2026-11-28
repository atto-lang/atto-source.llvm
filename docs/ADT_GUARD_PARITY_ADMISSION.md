# LLVM exact-source guarded ADT admission (not parity)

All 24 `.atto` fixtures are Git-blob-identical to Rust/Wasm PR #219 and canonical atto-platform-conformance PR #42.
The native admission runner rejects any invalid source that reaches real LLVM IR. For every admitted positive, the runner demands real native compiler output and **actual O0/O2 ELF guest execution** with exact exit and stdout. A positive refused by the compiler is recorded `UNSUPPORTED_SOURCE_NOT_PARITY`; a negative rejected by a generic parser or unsupported-feature diagnostic is **not** credited with semantic type checking.

Run `node tests/adt_guard_v1_native_admission.mjs --integrity-only` for SHA checks, then build the actual `oreslang-llvmc` and set `ORESLANG_LLVMC` to run the real native audit. LLVM source enum implementation is currently a separate, unmerged opt-in draft; this gate does not falsely promote that work to production.

**Still required for parity:** Option/Result constructor and payload AST, guarded match source lowering (bind only on matching tag *before* guard), lexical ownership checks, complete exhaustiveness and dead-arm proof, nominal enum integration, native guest execution plus sanitizers/actor boundaries.
