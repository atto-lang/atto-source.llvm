# AttoLang LLVM: path-sensitive source restoration CFG (v1)

**November 7, 2026 — draft / no guest promotion / source-owned stage.**

## Audit finding

The current LLVM ownership stack has the correct primitive fixed-point lattice (`OwnershipCfg.hpp`), a single-site `Reinitialize.write_id` checked by ownership admission (draft #1543), and a link from a source `FieldWrite` to a physical `StoreInst`. However, the existing production CFG builder primarily covers guarded-return paths and does **not** include all source statements' field moves, branch restorations, loop backedges, ordinary field reads, or conditional expression short-circuiting.

A single-site proof cannot establish that a later read is definitely initialized on **every** predecessor. Consequently, this patch adds an independent, *source-AST derived* full-statement field-state CFG for any compiler function where the checker minted a restoration witness.

## Production changes

`src/main/cpp/oreslang/types/OwnershipSourceFieldFlow.hpp` introduces `SourceFieldFlowBuilder`, which consumes the actual `Function` AST and an immutable snapshot of pre-statement class field ownership state. Its bounded graph supports:

- Direct scalar field `rt take` as `Move`, source reads as `Read`, a tracked whole-class read/move as `ReadObject`/`MoveObject`, and exact compiler-issued restoration tickets as `Reinitialize(write_id)`
- Ordered ordinary expressions and field assignments
- Real `if`/else CFG splits, lazily evaluated scalar conditionals, `&&` / `||` short-circuit source edges
- `for` index and `foreach` loop headers, exits and backedges
- Structured returns, early guard returns, and LIFO source field-defer effects at each exit
- A complete readback of source AST restoration tickets, including nested and unreachable source regions; **each minted ticket must appear exactly once in the actual CFG**, with no missing/duplicated source-site evidence

`validate_i64_ownership(Program&)` snapshots the checker-owned `Locals` before processing source statements. After the existing ownership admission and guarded-return graph, it invokes this new builder **only when a restoration ticket exists**. The existing `ownership::verify(Graph)` uses a powerset lattice: a read demands `possible == kOwned` and rejects merged `{Owned,Moved}`, e.g., a field restored on only one branch or only inside a loop that may execute zero times. This establishes conservative **all-predecessor field-state safety** for the admitted subset.

**Fail-closed source boundary:** Calls, class/record/sum constructors, capture-bearing or borrowed expressions, unproven alias roots, scoped class owners, opaque array commits, exceptions/catch cleanup, actor actions, task/suspension, and other effects not classified in this stage are rejected in functions containing restoration tickets. Existing functions without minted restorations are not promoted or changed by this new source-flow gate. Existing guest NLL stays closed.

**Not a full LLVM dominator/SSA proof:** The new graph represents all statements in a function's *restricted class-field projection*, not all language semantics or typed MIR loan use-def completeness. An individual branch store need not dominate the join if mutually exclusive branches both reinitialize; what is required is that every path to a subsequent read has an admitted restoration. This verifier does NOT prove optimized LLVM instruction dominance, compiler stable instruction generation after optimization, complete loan-use inventories, actor runtime safety or cross-backend guest parity. Its outputs hard-code `source_loan_use_complete=false`, `optimized_llvm_dominance_verified=false`, `guest_verified=false`.

## Testing and exact-head receipts

- `tests/native_source_field_cfg_96_test.cpp`: **96 authored C++ source/CFG assertions**, including 16 explicit controls (straight-line move/reinit/read, one-vs-both-branch reinit, duplicate/forged tickets, terminal predecessors, loop restoration/zero-trip and unsupported effects) and 80 deterministic per-projection path mutations covering ten distinct field names and eight path shapes. Uses real `OwnershipCfg::verify`; **host C++ only**.
- New named CTest `native_ownership_source_field_cfg_96` is the **seventeenth** focused native ownership CTest. The prior 16 cases, twenty scoped compiler checks, and fifty actual LLVM/Clang `.atto` fixtures remain required.
- CI source shape test suite `tests/ownership_ci_workflow_shape_38.test.mjs` has 38 fail-closed tests including missing/duplicate seventeenth CTest inventory/build target and missing source/header triggers. Fetched exact-source isolated V8/Node assertion shim: **38/38 PASS**, status `OWNERSHIP_WORKFLOW_SHAPE_ONLY_NOT_EXECUTION`, `checkedNativeCTestCount=17`, `verified=false`, `guestExecutionVerified=false`. No hosted Node 22 or native compilation occurred.
- Reusable graph has 2,048-block, 16,384-operation, depth-64 and bounded fanout limits; unknown effects and overflow fail closed.

## Merge hold and next proof layer

1. **Runner admission:** GitHub source-org ownership jobs keep failing with `steps=null`. The authorized billing owner must diagnose documented failed-account-payments **or** Actions-spend-limit eligibility, run a source-free same-org canary with real nonzero steps, then execute immutable-head 38 hosted Node tests, 17 native CTests with ASan/UBSan, 20 scoped compiler tests and 50 real compiled guest fixtures. Record head/tree/JUnit and negative mutation receipts.
2. **SSA and real optimized IR:** Compiler-authenticate every LoanId/use (loads, calls, captures, phi, exception, cancellation, suspension), demonstrate full source-origin-to-typed-SSA coverage, stable generation-fenced LLVM store identity and dominance after optimization, then enable only validated guest subsets.
3. **Cross-backend:** Match canonical `.atto` results across Java/GraalVM, LLVM, .NET NativeAOT, Wasm and Forge/Cranelift.
4. **Review:** Keep draft, obtain independent `ORESoftware` approval (or eligible independent `the1mills`), reconcile stacked PRs semantically and enforce exact-head stepful CI before merge. Do not mirror private source across orgs or bypass review/security gates.
