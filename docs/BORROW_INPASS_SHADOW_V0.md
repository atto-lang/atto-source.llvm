# LLVM native same-pass borrow shadow V0

This stacked draft extends the real `OwnershipDataflowV1::analyze` call with an **optional callback** that is invoked for each **successfully accepted** typed CFG operation and normal exit. It does not change the existing ownership dataflow algorithm or allocate when no callback is provided. The restricted `OwnershipSourceLoweringV1` path remains integrated with the same native analysis.

The callback is intentionally not a canonical owner proof or complete IR. Events contain stable CFG operation positions, local owner indexes and enum kinds, but no trusted Copy/Send/Share trait witnesses, loan IDs, actor generation, exceptional/cancel/await paths, or guaranteed cross-backend IDs. The Rust verifier in `atto-borrow-checker.rs` must not silently accept these events as validated typed MIR. Mapping into the shared versioned IR requires a producer with explicit semantics and bounded, complete operation handling.

Any callback exception escapes before the compiler can obtain an accepted result. Normal compilation calls `analyze(graph)` with no callback and has no extra traversal.

Run `ctest -R native_ownership_shadow_v0 --output-on-failure` plus `native_ownership_dataflow_v1` and `native_ownership_source_lowering_v1` from this exact head on an admitted runner. Compile with GCC/Clang sanitizer variants and executed .atto guest tests before making any stronger parity claim. A zero-step Actions failure is NOT_EXECUTED.
