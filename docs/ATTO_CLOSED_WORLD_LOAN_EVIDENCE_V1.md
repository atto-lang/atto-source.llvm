# AttoLang ownership: closed-world NLL and write-ticket fences

**DRAFT. Production LLVM native CTest and compiler-backed guest tests have not executed. This is not sound source-level NLL.**

## Production CFG hardening

`OwnershipCfg.hpp` now carries a separate optional `write_id` on `Operation`. The `Reinitialize` transition rejects absent/zero write-site IDs, mixed loan-ID metadata, duplicate write tickets anywhere in the CFG (including currently unreachable blocks), and write metadata on any other operation. The existing move-state and live-loan preconditions still apply. **This ticket is only a required compiler-generated claim, not a proof that a guest field assignment actually occurred.** No source-to-guest lowering is permitted to invoke this transition without a separately established MIR write and the matching emission receipt.

`OwnershipLastUse.hpp` rejects all unclassified `Read` and `ReadObject` operations that follow a tracked lexical borrow, including owner reads that might be safe. This deliberate loss of precision prevents an omitted borrowed-use annotation from allowing premature release. `lower_closed_world_loan_tokens_v1` derives the last `UseSharedLoan`/`UseExclusiveLoan` offsets from the bounded one-block token operation stream; it rejects duplicate, missing, mismatched, unminted or manually released token histories. Its result is passed through `lower_explicit_last_use_evidence_v1`, which inserts releases and reruns the regular ownership CFG verifier. This derived stream is **not authenticated SSA completeness** and is not called by guest lowering.

## Tests

`tests/native_ownership_nll_closed_world_16_test.cpp`: 16 new host C++ negative/positive cases for distinct write tickets, zero/missing ticket rejection, duplicate write-site denial, loan-token impersonation, unclassified ordinary reads and eventual typed release.

`tests/native_ownership_derived_last_use_12_test.cpp`: 12 host C++ cases for token-derived final use, repeated uses, multi-token streams, wrong modes, mixed releases, loops and opaque reads.

The original 30 C++ last-use cases have been reconciled with required explicit write tickets and the conservative read fence. CMake registers both new tests, and the exact-head source-owned LLVM workflow now requires **eight focused native CTests**, in addition to previously authored scoped `.atto` compiler fixtures and the real 50-program LLVM/Clang guest matrix.

**Evidence limitation:** all these native compiler/test cases are authored but **NOT_EXECUTED** because the source-org Actions jobs have zero steps. Never promote a CTest inventory check or source-only analysis to source-to-guest parity. Require immutable head/tree, nonzero executed steps, C++ compilation against real production headers, actual negative semantic diagnostics, LLVM IR+Clang guest receipts, and independent review before merging. No private source copied to another organization.

## Next compiler work

Generate the complete tokenized use-def stream from typed SSA; fence opaque call/capture/exception/cancellation edges; emit actual writes for restored fields; prove ActorRef/tenant/suspension generation ownership. General NLL through branch/loop joins remains unimplemented.

## Ticket/loan identity-domain follow-up (stacked draft)

`Reinitialize.write_id` cannot alias any lexical explicit/derived loan origin or any entry loan ID. The preflight checks unreachable blocks as well. Six new host C++ cases are registered as a tenth focused native CTest. These tests are **authored but NOT_EXECUTED** and cannot authenticate a real MIR field write, complete SSA uses or guest NLL parity.


## Source-only typed-MIR SSA/use and store-claim bridge (nonpromoting)

OwnershipMirEvidence.hpp adds an opt-in, one-block, caller-supplied typed-MIR inventory comparison. Each UseSharedLoan/UseExclusiveLoan CFG operation must match exactly one typed SSA use-site claim and one lowered edge, with identical LoanId, place, CFG site, SSA value, use class and site ID. Omitted, duplicated and forged use metadata fail closed. Call, capture, phi and send categories are classified but not independently authenticated. Exception unwind, cancellation and suspension use categories remain rejected pending cleanup and lifetime evidence.

Every Reinitialize.write_id must separately match exactly one typed-MIR field-write claim and one native-store claim with identical place and stored SSA value; phi-only, missing, duplicated and misdirected store claims fail closed. The adapter calls the real CFG verifier, but two matching caller-supplied streams can still omit real uses or fabricate emission. Therefore it **always returns verified=false and compiler_backed=false** and is not invoked by guest lowering.

30 additional native C++ cases are authored, registered as the eleventh focused CTest, and require an exact-head CTest execution with no failures or skips. The workflow also repairs an inherited malformed grep inventory command. All 30 tests are AUTHORED, NOT EXECUTED until actual source-org runner steps occur. Neither this bridge nor a store-shaped record proves emitted LLVM IR or native AttoLang guest behavior.


## 2026-11-07 opaque-effect typed-MIR audit hardening

The stage-zero typed-MIR checker previously accepted unclassified Read,
ReadObject and Suspend effects after a borrow, allowing both caller-supplied
inventories to agree while jointly omitting an actual borrowed use. It now
rejects such effects after any lexical or live entry loan, including after
explicit release, until SSA provenance and continuation ownership are proven.
Reads and suspensions before any loan remain unverified source candidates.
One-block PhiEdge uses are rejected because no predecessor edge is tracked.
Entry map size is bounded at 4096 and place names at 256 characters for
the CFG and all use/write/store candidate inventories.

Eighteen additional positive/negative cases are authored, including entry
loan evasion and bounded metadata. An existing phi-positive case becomes a
negative case. The exact-head source-owned workflow registers 12 focused
native CTests and requires the exact 12-name JUnit set without failures,
errors, skips or duplicate test names.

All new cases are AUTHORING EVIDENCE ONLY / NOT_EXECUTED until a real
source-organization runner executes nonzero steps. The adapter remains
verified=false and compiler_backed=false; no guest lowering is enabled.


## Actual LLVM instruction-derived SSA/store census (not a guest proof)

`backend/NativeIrOwnershipCensus.hpp` walks actual emitted LLVM Module basic blocks after LLVM verification and before printing LLVM IR. Its StoreInst records bind the real instruction identity, destination pointer, stored LLVM SSA value and deterministic per-walk ordinal. It independently counts SSA operands and reverse use-list edges, LoadInst, CallBase, PHINode and phi inputs; cross-function SSA users fail closed. Cached store entries cannot authenticate changed operands. The result is compiled and invoked in the real `LlvmCodegen.cpp` output path (not caller-provided JSON).

**Non-promoting scope:** no mapping from source LoanId/use-def to LLVM instruction ID is proven. No Reinitialize.write_id is yet bound to a particular emitted store. Every result remains ownership_complete=false and guest_verified=false; this cannot enable guest NLL. Raw LLVM pointers must not outlive their Module.

30 new host C++ test assertions construct real in-memory LLVM IR, including mutated stores, stale evidence, calls, actual phi nodes, foreign-function SSA edge rejection and a no-store empty module. The test is registered as the thirteenth focused native CTest; the existing real 50-program LLVM/Clang guest matrix remains separately required.

CI audit: the previously committed source-owned workflow had nine copies of the compiler-matrix/final-head sequence and broken injected inventory grep text. This version keeps one complete ordered sequence, repairs the inventory line, and requires the exact 13-case JUnit testcase-name set. All these tests remain NOT_EXECUTED pending actual source-organization runner admission.


## Compiler-emitted FieldWrite → LLVM StoreInst ledger (2026-11-07)

The real `LlvmCodegen.cpp` now mints a per-module provenance ticket for each emitted source `Statement::Kind::FieldWrite`, including direct scalar and owned-array field replacement. A scoped ledger binds each source statement identity and byte offset, `owner.field` place, actual `StoreInst` pointer, destination pointer and stored SSA value. The compiler attaches `!atto.ownership.field_write.source.v1` metadata to actual LLVM stores with source place, minted ticket and source offset, and rechecks physical instruction membership, operands and metadata after LLVM module verification and before emitting IR. Missing/replaced/stale stores, source mutations, duplicate source/store events and corrupted metadata fail closed. All evidence is minted within production lowering, not supplied by external callers or JSON receipts.

This is a **pre-optimization direct field-store** bound, not complete typed-MIR/SSA or guest ownership proof. Deferred cleanup, branch-coverage exhaustiveness, source `LoanId` and CFG `Reinitialize.write_id` correlation, optimization survival, native machine-code stores and guest execution remain unproven. Empty ledgers do not certify any source-to-IR binding. `ownership_complete=false`, `optimized_code_verified=false` and `guest_verified=false` remain mandatory.

The new host-linked LLVM test has 75 assertions: 59 negative mutations (source kinds, source fields, paths, duplicates, erased stores, changing SSA destination/value, missing/bogus metadata) and 16 positive/nonpromoting controls. It is registered as focused CTest #14. Because the inherited focused shell block still had duplicated and malformed interior fragments, its entire test stage is rebuilt with an exact 14-name inventory, JUnit testcase set, zero failures/errors/skips and one explicit executed marker. The existing compiler-backed 50-case `.atto`/Clang fixture stage remains separate. Tests remain AUTHORED / NOT_EXECUTED until Actions can admit a source-organization runner.
