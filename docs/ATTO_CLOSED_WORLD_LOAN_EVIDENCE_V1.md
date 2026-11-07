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


## Compiler-owned direct FieldWrite coverage (2026-11-07)

The bounded compiler now independently scans the native Program's Function
source AST for every direct Statement::Kind::FieldWrite, recursively visiting
both conditional arms and nested loop/branch source statements, as well as
the supported caught-throw source field-write cleanup list. This source
inventory is assembled from the Program, **not** from the LLVM emitter's
StoreInst ledger or external JSON receipts.

At emission, each supported direct field write records the source Function
address and the currently active LLVM Function as well as the earlier
source Statement/StoreInst provenance. The compiler validates the actual
LLVM Module and then requires an exact source-AST-to-ledger bijection:
no omitted, forged, foreign-function, duplicate or stale direct field
write may pass. The per-source compiler-owned emission identity is rechecked
against the LLVM function containing the physical StoreInst.

This is **direct source FieldWrite completeness only**. Other LLVM stores,
Defer-kind field effects, all source LoanId uses, CFG Reinitialize.write_id
pairing, full ownership CFG branch effects, optimized machine code and guest
execution are NOT certified. A source without direct field writes is a
vacuous inventory, not a positive source-store authority receipt. The
source ledger maintains ownership_complete=false,
optimized_code_verified=false and guest_verified=false.

The accompanying native LLVM CTest adds 50 new authored assertions:
42 fail-closed mutations for missing writes in straight-line, nested
then/else, loop and caught-throw source paths, wrong source/function identity,
forged ledger-only entries, changed values and bounded malformed inventories;
plus eight non-promoting controls. Focused ownership CI requires exactly
15 distinct named CTests and 15 distinct passing JUnit cases, followed by
the separately guarded real 50-program LLVM/Clang guest matrix. Source-org
runner-admission failures mean these remain AUTHORED, NOT_EXECUTED.


## Independent native function provenance and reverse metadata inventory

The direct-source field-write coverage verifier now requires a **separate
compiler declaration registry** mapping every `Program.functions[]` identity
to its actual, fully emitted LLVM function. A valid StoreInst must belong to
the native function independently associated with its source function. This
prevents the emitter from passing a self-consistent but wrong function pair.
Missing/foreign/aliased declarations fail closed, including missing source
functions with zero direct field writes.

A second independent scan of the actual LLVM module inventories **every**
StoreInst carrying `atto.ownership.field_write.source.v1`. The reverse
tagged-store inventory must match the source ledger exactly. Unexpected
cloned/forged/unpaired tagged stores fail closed, while ordinary untagged
compiler housekeeping stores remain permitted. An empty source/ledger
inventory cannot set `direct_source_writes_complete=true`.

The original field-write coverage host test expands to **66 assertions**:
54 negative mutations and 12 positive/nonpromoting controls, including
forged native declaration maps and extra tagged stores. The exact-name
focused CTest remains one of 15 and the 50 real compiler-backed `.atto`
fixtures remain a separate gate. No compiler-backed guest or hosted test
success is claimed until nonzero Actions runner steps exist. General LoanId,
Reinitialize.write_id, optimized-code, cancellation and guest correctness
remain unproven and all broad proof flags stay false.

## Fail-closed source pointer lifetime fence (2026-11-07)

A security review found that verify_source_coverage previously invoked
the ledger's verify(module) before enumerating the current source AST.
Since verify(module) dereferences FieldWriteSite.source, a deleted or
replaced Program/Statement could cause a dangling source pointer read.
The production verifier now inventories the current live AST first, then
checks saved identities by pointer comparison only against that inventory,
and cross-checks the independent source-to-native declaration registry.
Only once identities are established does it inspect stored source fields
and emitted LLVM instructions. The StoreInst membership scan similarly
precedes use of store operands. This ordering is a lifetime safety fence,
not a solution to ABA pointer-address reuse: the compiler must preserve
stable, immutable AST nodes during codegen and cannot claim general NLL.

The native host-LLVM regression test expands to 74 authored assertions:
60 negative mutations and 14 positive/nonpromoting controls. New cases
delete/replace source nodes after binding and cover multiple branch and
caught-cleanup field stores. The focused source coverage CTest is renamed
to end in _74. The workflow still requires exactly 15 native CTests and
a separately checked 50-program compiler/Clang guest matrix.
All native tests remain AUTHORED / NOT_EXECUTED until actual runner admission.
Ownership completeness, optimized code and guest proof flags remain false.

## Validated compiler source node identities — 2026-11-07

The parser/resolver mints a process-unique compilation-unit epoch after
ownership and type validation succeed, with immutable local function/node
identities, sealed function names, structured root/then/else/cleanup paths,
and source function/node counts. Zero identities are unvalidated.
Codegen now instantiates FieldWriteLedger from the already validated Program,
not the host-only default constructor. The production verifier validates
the sealed program before using a saved Statement pointer and additionally
compares node IDs, source function IDs, epoch and structural route against
the emission-time snapshots. This catches ordinary allocator-address reuse,
source truncation, structural branch reparenting, and duplicate/missing
source IDs, without relying solely on raw pointer equality or offsets.

Source ID correctness is a trusted compiler-internal invariant, not a
cryptographic defense against arbitrary C++ memory corruption. Mutation of
a validated AST while emitting is still unsupported. The SourceNodeIdentity
host fixture adds 65 assertions, including 45 negative mutations and 20
nonpromoting controls. The focused CTest matrix now requires 16 distinct
passing tests with exact inventory/JUnit case matching and zero skips.
The separate 50 real AttoLang compiler/Clang guest fixtures remain required.

This work does not bind complete loan-use provenance or moved-field
Reinitialize.write_id across typed MIR and emitted native code. Those
remain closed pending a separately compiler-backed proof. No optimized
machine-code or guest execution proof is promoted, and all broad ownership
and guest flags remain false. Until actual source-org runner admission
executes nonzero steps, new assertions are AUTHORED / NOT_EXECUTED.

## Candidate typed CFG Reinitialize.write_id ↔ real StoreInst bridge (2026-11-07)

New NativeIrReinitializeWriteBridge.hpp supplies a fail-closed, host-level
candidate verification function. It first verifies the independent
source-to-Llvm-StoreInst exact ledger, then independently runs the existing
ownership CFG fixed-point validator and checks every reachable Reinitialize
write_id against a compiler-minted StoreInst ticket, the same exact source
Function, the emitted LLVM Function and canonical field place. It rejects
zero, missing, repeated, unreachable, forged, wrong-field and wrong-function
claims. No candidate is allowed to invent the ticket or substitute a
source-only marker for a real live StoreInst.

IMPORTANT: The source compiler does not yet publish a complete typed MIR CFG
with all moved-field reinitialization operations to codegen. This new
bridge is NOT called by production emit_verified_llvm_ir; the generated
graph fixtures are synthetic host checks only. A candidate bridge can
validate a supplied reinit claim, but cannot prove that no other source
reinitialization was omitted. It also does not prove LoanId/SSA use closure,
optimization behavior, cancellation or guest parity. Every wide-scope
proof flag remains false and no guest execution is claimed.

The existing LLVM source-field-write host CTest is expanded from 74 to
114 authored assertions (88 negative and 26 positive/nonpromoting), adding
40 cases for candidate write-ID binding. Its registered test name and
build target now end in _114. There are still precisely 16 focused CTests
with exact inventory/JUnit gate and a separate 50 compiler-backed AttoLang
/Clang fixture matrix. No test is marked executed until a source-org
runner has admitted the exact head and recorded nonzero steps.

## Verified-source tree-shake reissue and candidate native-store ticket audit

CLI compatibility audit discovered a real integration regression:
`--tree-shake` validates and emits the whole Program, then removes
unreachable functions and emits a second time. That removal invalidates
new compilation-unit, function and Statement IDs. The compiler now
validates the original seal, retains only a compiler-computed subset
of original sealed function IDs, and reissues a fresh compilation-unit
epoch, contiguous function IDs, structural paths and source counts.
Pruning failure revokes the unit's codegen authority; arbitrary
post-validation source mutation still fails closed. This is a trusted
compiler-only transformation, not an input-controlled resealing escape.

Source-node host test expanded to 85 authored cases: 57 negative
mutations and 28 nonpromoting controls, adding pre-corruption,
unknown retained IDs, both keep/drop directions, nested path,
cleanup identity and consecutive reissue tests. The other direct
source-store host test has 114 authored cases (88 negative, 26
controls), including candidate CFG-to-LLVM Reinitialize.write_id
correlation tests. Both tests are covered by the same exact 16-CTest
inventory and the independent 50-program compiler guest matrix.

No production MIR code currently supplies the complete reinitialization
CFG to the candidate store bridge. Accordingly typed-MIR completeness,
guest execution, LoanId coverage and optimizer correctness remain
explicitly false/unproven. All new cases are authored, NOT_EXECUTED.

### Production compiler/Clang tree-shake identity reseal gate

The previously existing `native_tree_shaking_direct_calls` CTest now runs
as its own mandatory executed gate in the ownership workflow, separate
from 16 focused host CTests and the 50 real ownership compiler fixtures.
It exercises a new source where an unreachable function *precedes* main
and an invoked helper; deleting the earlier function forces new source
FunctionId and structural NodeId paths before the second LLVM emission.
The script requires successful production compiler output, no dead symbol,
a retained helper, a real Clang-linked binary and exit code 42.
The marker `ATTO_TREE_SHAKE_SOURCE_RESEAL=COMPILER_CLANG_GUEST_EXECUTED`
and CTest JUnit receipt are checked on exact head. A CTest-registration
only check or host-only model run must not substitute for actual execution.
No successful run has been observed; current runner-admission failures
leave the guest claim explicitly UNVERIFIED.
