# LLVM ownership: live instruction readback and 14-CTest workflow reconciliation

**November 7, 2026 — NON-PROMOTING.** This patch is stacked on [LLVM #1532](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1532), which already collects **actual LLVM Module/Instruction-derived** SSA/store census data in production codegen. This change does not fabricate source LoanId/field-write authority, and does not enable NLL guest lowering.

## Audited gap in real LLVM census

The previous `NativeIrUseStoreCensus::contains_store` checked the cached `StoreInst*` address and then called `identity->getPointerOperand()` and `identity->getValueOperand()`. An LLVM pass can call `eraseFromParent()` on that instruction, leaving the cached pointer dangling. Using it for IR readback risks a use-after-free; additionally, a cached receipt did not invalidate when another module instruction was inserted or deleted.

## Implementation

- `collect_native_ir_ssa_census(const llvm::Module&)` now records its **borrowed** source Module pointer, alongside the existing actual LLVM store and SSA census.
- `contains_store` does **not dereference the caller-provided instruction pointer**. It walks the still-live source Module and compares live `StoreInst` identities, previously captured ordinal, destination and stored SSA value; it requires the full instruction/store counts to agree before returning a matched claim. Added/deleted/reordered instructions and changed physical store operands invalidate cached readback.
- This does **not** create a stable identity under memory allocator ABA, guarantee a snapshot under concurrent mutation, nor allow a census to outlive its Module. Caller must rederive the census after an LLVM pass. A matching emitted store does not authenticate an AttoLang source field assignment or borrowed-use closure.
- Added `tests/native_ir_store_live_readback_14_test.cpp` exercising real LLVM IR including added/deleted instructions, erased `StoreInst` readback without dereference, operand tampering, a foreign Module and fail-closed guest/ownership flags. Registered as an independent **fourteenth native ownership CTest** in CMake.
- Reconciled the integrity checker from sibling [#1531](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1531) semantically with #1532's **thirteen** native cases. The checked workflow now requires fourteen CTests, the existing twenty scoped compiler CTests and fifty distinct real LLVM/Clang `.atto` fixture runs, with exact-head/tree checkout.
- Added `scripts/ownership-ci-workflow-shape.mjs`, `bin/check-ownership-workflow-shape.mjs`, and `tests/ownership_ci_workflow_shape_23.test.mjs`: fail-closed source-only tests for malformed, duplicated, stale, missing, or counterfeit workflow gates. Execute Node `--check`, 23 exact TAP test groups and the checker in the source-org workflow before native installation.

## Evidence and nonclaims

**Verified in isolated V8 (not hosted Node): 23/23** fetched exact-source static-workflow test groups passed; the checker reports `structurallyValid=true`, `verified=false`, `guestExecutionVerified=false`, native CTest expected count = **14**. This is only source-model evidence. The new fourteen cases of real LLVM IR are **AUTHORED / NOT_EXECUTED**; no installed native LLVM development headers/toolchain were available for local C++ execution, and source-org GitHub Actions jobs have been failing before any runner steps. The real compiler 50-program matrix is **NOT_EXECUTED** on this head. No use-def source completeness, actual source-field write mapping, or five-backend parity is established.

## Release and security gates

The previously captured GitHub provider annotation in [conformance issue #315](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/315) attributes source-org admission to failed recent account payments **or** insufficient Actions spending limit. Authorized billing owner must diagnose and resolve this; execute same-org private/source-free canary with a nonzero runner and steps, then run the exact immutable head through Node 23/23, native 14/14 CTests, twenty scoped compiler checks, fifty compiled guest fixtures and sanitizer mutations. Require independent `ORESoftware` approval. Do not export private source across organizations, relax review requirements, or call emitted-IR census full ownership proof.
