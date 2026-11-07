# AttoLang LLVM moved-field restoration witness (v1)

**2026-11-07 — draft, fail-closed, non-promoting.**

## Audited real gap

The production `validate_i64_ownership(Program&)` checker tracks `LocalOwnership::moved_fields` when checking a `Statement::Kind::FieldWrite`. Its existing general `OwnershipCfg` is built around guarded-return paths and **does not generate a compiler-linked `Reinitialize.write_id`** for source field restorations. The LLVM source field-store ledger introduced in [#1538](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1538) records real stores but previously had no per-site restoration ticket to reconcile.

## Implemented narrowly — checker → one-site CFG → live store

- Extend `Statement` with an **appended** `reinitialize_write_id` field, zero by default. It remains zero on ordinary writes. This is a compiler-internal transient witness, not a serialized source annotation or stable optimization handle.
- Before validation, recursively revoke every previous witness, including branch/loop statements and catch cleanup. A `RestorationFailureFence` scope guard clears **all** witnesses if ANY ownership validation or publication fails, rather than leaving partial authorization.
- When the ordinary ownership checker encounters a **definitely moved scalar class field** and admits a write after validating all ownership/borrow constraints and RHS effects, it mints a unique bounded restoration ticket `(1 << 63) | ordinal` (1…4096 across the whole checked program). The high domain is intentionally distinct from ordinary loan/use numeric offsets.
- The checker builds a one-entry, one-operation `ownership::Graph`: source projection initially `kMoved`, one `Operation::Reinitialize(place, write_id)`. The existing `ownership::verify(Graph)` must admit it before the witness is published. **This models only the one-site restoration transition**, not the full function CFG.
- Production `LlvmCodegen.cpp` passes this exact checked source witness to the `CompilerFieldWriteLinks::record` invocation adjacent to actual `CreateStore` for the matching scalar FieldWrite. The ledger enforces nonzero/high-bit bounded identity, no duplicate restoration tickets, direct-write-only classification, canonical field projection and the real LLVM function/destination/value.
- After `llvm::verifyModule`, the existing live-only bounded one-pass store census rechecks actual instruction existence and expected operands. Source / compiler evidence flags remain false for whole-source ownership and guest verification. No LLVM optimizer may reuse this ephemeral pointer/identity without recomputing evidence.

## Added negative and positive tests

- `tests/native_reinitialize_store_witness_48_test.cpp`: one native CTest with **48 authored real-LLVM host checks**: baseline actual LLVM IR, one-site moved-field CFG, ticket and SSA/destination correspondence, 8 low-domain forgeries, 8 high-domain quota overruns, 8 bogus deferred/catch categories, 8 noncanonical/oversized field projections and 8 independent CFG/LLVM mutation probes (zero ticket, unmoved, wrong field, duplicate identity, foreign function, changed stored SSA value, inserted and erased instruction). Test code is authored, **NOT_EXECUTED**.
- The source-owned CI workflow requires **16 distinct native ownership CTests**, no failed/skipped/missing JUnit cases, the existing twenty scoped compiler checks and fifty actual LLVM/Clang `.atto` guest fixtures.
- `tests/ownership_ci_workflow_shape_33.test.mjs` covers 33 source-only integrity regressions including the sixteenth CTest, exact branch/path watches, source head and JUnit bounds. **33/33 passed in isolated V8 with a lightweight Node test/assert shim**. This does not constitute hosted Node 22 or LLVM native execution; the static checker always returns `verified=false` and `guestExecutionVerified=false`.

## Deliberate nonclaims / next acceptance gates

This is **NOT** full source-to-typed SSA use-def closure, dominance proof across all CFG edges, proof of a complete program-wide inventory of Reinitialize operations, nor a source-level borrow lifetime guarantee. Defers, caught-exception field effects and actor field operations still have real store links but no moved-field restoration witness. A `Statement` can be mutated after checking by a host caller; this prototype depends on the existing compiler pipeline's checked-AST integrity and does not introduce a cryptographic binding or stable optimized IR identity. Do not authorize moved-field NLL guest execution or claim Java/.NET/Wasm/Forge parity based on this one-site result.

P0: GitHub source-org Actions runner admission still fails with `steps=null` due to the documented provider annotation indicating recent account payments failed **or** the Actions spending limit must increase. Only the authorized billing owner can resolve the precise cause. Require a source-free same-org canary with nonzero executed steps, then exact-head 33 hosted Node checks, 16 non-skipped CTests (plus ASan/UBSan), twenty scoped compiler checks and fifty **actually compiled** LLVM/Clang `.atto` guest fixtures, retaining head/tree and test receipts.

P1: Thread compiler-generated source-place origin and full typed SSA loan uses through all paths (calls, closures, phi, exception, cancellation, suspension), prove dominance of every restoration store and generation-fenced identity under optimization. Obtain independent `ORESoftware` or eligible `the1mills` approval before any ready/merge action. No billing mutation, cross-organization private-source mirror or security exception is authorized.
