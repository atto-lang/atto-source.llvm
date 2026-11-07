# AttoLang / LLVM source field-write → emitted StoreInst v1

**November 7, 2026 — advisory / non-promoting / draft.**

## Concrete compiler change

The source backend already owns actual `llvm::StoreInst` emission, but LLVM PR #1532 only counted instructions. PR #1533 hardened cached-store readback against erased instructions. Neither retained a **compiler-minted link from a checked source field-write lowering site** to the physical emitted instruction.

This change adds `backend/NativeSourceFieldWriteLinks.hpp` and wires `CompilerFieldWriteLinks::record(...)` into real `emit_verified_llvm_ir` FieldWrite emission, not a caller-provided manifest. It records the source projection key, the LLVM Function, actual emitted StoreInst identity/destination/SSA value and a sequential emission-site identity minted during lowering. Coverage in this version:

- **Direct:** `Statement::Kind::FieldWrite` to ordinary scalar class fields and checked direct `rt take` array handle field replacements
- **Deferred:** field assignments executed by the function's deferred-effect cleanup loop
- **CatchCleanup:** caught-throw deferred field assignments executed along its actual unwind CFG

Following `llvm::verifyModule`, `collect_native_ir_ssa_census(module)` inspects real instructions; the compiler-owned field ledger requires each recorded site to remain a distinct, live physical `StoreInst` in the attributed function with the same destination and value. Duplicate claims, stale/erased/reordered physical stores, foreign module/function, invalid projection keys, and mismatched SSA operands **fail closed**. The site id cannot be supplied by `.atto` input.

## Explicit limits — no ownership promotion

This is a **subset** of source-to-IR write linking for production user-function field mutations. It is not a completeness proof for all expression reads, actor field operations, indirect writes, ownership cleanup paths, or emitted stores outside these three field-write categories. It is not a compiler-authenticated correspondence to `OwnershipCfg::Operation::Reinitialize.write_id`. It does not derive SSA loan use-def closure or validate borrow lifetime, dominators, exceptional exits, cancellation, suspension or full source→guest ownership semantics.

A link is valid only while its owning LLVM Module lives, with no concurrent mutation, and while its collected physical instruction census is current. No durable instruction identity across optimization generation/allocator ABA is provided. The validator returns `source_ownership_complete=false` and `guest_verified=false` even when all recorded stores match.

## Test gates

- `tests/native_source_field_write_link_26_test.cpp`: 26 authored host LLVM-IR C++ cases for real StoreInst source links, minted identities, exact projections, direct/defer/catch origin tags, stale operand changes, duplicate claims, foreign module/function, added and deleted instructions, negative ownership and guest flags.
- CMake native CTest name: `native_ownership_source_field_write_link_26`. This is the **15th distinct ownership CTest**.
- Exact-head workflow still includes 20 scoped compiler checks and the separate **50 actual LLVM/Clang `.atto` compiler fixtures**. It now also checks 28 source-only workflow-shape regressions and its five new negative mutations for the fifteenth CTest and path/branch triggers.
- Fetched exact source static integrity suite: **28/28** passed in isolated V8 with a lightweight Node test/assert shim. The `inspectOwnershipWorkflowShape` result is `structurallyValid=true`, `checkedNativeCTestCount=15`, `verified=false`, `guestExecutionVerified=false`. This is NOT hosted Node or an executed C++ test. The native LLVM development headers/toolchain are absent from the local work environment, and source-org Actions have been failing before runner admission.

## Required further proofs

1. Restore source-org Actions runner admission via **authorized** GitHub billing owner; the provider annotation tracked in conformance issue #315 reports failed recent payments **or** insufficient spending limit.
2. Run source-free source-org canary with allocated runner and nonzero steps, then exact-head source-owned 28 Node checks, all 15 native ownership CTests and sanitizer cases, twenty scoped compiler checks, fifty compiler/Clang guest fixtures and negative mutations. Capture immutable head, tree and JUnit/compiler receipts.
3. Introduce compiler-minted `Reinitialize.write_id` linkage with full typed SSA use/projection/dominance proofs; run equivalent `.atto` guest suites for Java/GraalVM, LLVM, .NET NativeAOT, Wasm and Forge/Cranelift.
4. Obtain eligible independent `ORESoftware` or `the1mills` PR approval. Do not merge based on source-model test success, weaken security rules, or export private source across organizations.
