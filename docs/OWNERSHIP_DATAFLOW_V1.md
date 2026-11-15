# OwnershipDataflowV1 — bounded compiler-time CFG ownership verifier

**Status:** initial conservative analysis, integrated with the restricted ActorRegionV2 guarded-return lowering. It is not yet a general source-language borrow checker or proof of whole-program zero-GC.

## Model

The analysis takes a compiler-owned CFG with unique owner identities and explicit operations:

- `Create`: initialize a previously uninitialized owner.
- `Use`: read a live owned value; it is allowed under shared loans but not a mutable loan.
- `UseMutable`: mutate through the owner only when no shared or mutable loan exists.
- `UseThroughMutableBorrow`: mutate through an active exclusive mutable loan.
- `BorrowShared` / `EndBorrowShared`: maintain a bounded shared-loan count.
- `BorrowMutable` / `EndBorrowMutable`: enforce exclusive mutable loan.
- `Take(source, destination)`: transfer the live resource into a distinct uninitialized owner; the source becomes moved and the destination inherits the original acquisition order.
- `Drop`: explicitly destroy once; a later use/drop is rejected.
- `ExitNormal`: emit a reverse acquisition-order cleanup plan for all still owned objects.

Every reachable CFG block must have successors or an explicit exit. Join points require **identical** owner states and borrow counts on every incoming edge. This safely rejects differing branch ownership until a future path-sensitive/SSA ownership representation can prove and reconcile it.

Bounds are explicit: 64 owners, 4,096 blocks, 16,384 edges and 1,024 operations per block. Unknown operations, invalid successors, unbalanced borrows, duplicate identities, all reachable CFGs must be acyclic in v1, and non-normal exits fail closed. A cycle is rejected even if another branch reaches a normal exit; a separate bounded-loop termination proof is required before cyclic CFGs can be admitted.

## Current integration

ActorRegionV2 constructs a small CFG: all lexical objects are created in the entry block, then both guard outcomes reach separate normal-exit blocks. The analysis derives the reverse destruction list, which the LLVM emitter uses for its shared cleanup block. The compiler still admits only one pure equality guard, at most eight fresh immutable class owners, no actor scheduling, methods, nested scopes, exceptions, await, cancellation or ownership transfer in the source language.

## Tests

`native_ownership_dataflow_v1` contains sixteen positive scenarios and thirty-nine negative scenarios. It checks cleanup order, moved and explicitly dropped owners, shared/mutable borrow release, double destruction, use after move, dropping with a loan, conflicting aliases, mismatched join states, suspend exit, missing explicit exit and duplicate owner identities, unbounded cycles, missing/invalid transfer destinations, transfer into an initialized destination, use-before-create, unknown operation kinds, unreachable blocks, unbalanced borrows, mutable access during shared borrowing, incompatible acquisition histories at joins, ending a nonexistent loan, stray transfer metadata, reading through the owner during an active mutable loan, and invalid exit operations.

The negative fixtures assert their exact expected ownership diagnostic (rather than accepting
any rejection). Valid take-to-destination fixtures isolate use-after-move and CFG join
mismatch. Additional negatives separately reject exceptional and cancellation exits,
transfer during an outstanding shared loan, mutation through a missing exclusive
loan, and ending a nonexistent mutable loan.

The native test binary now uses an explicit named-case dispatcher. It executes every
fixture even if an earlier fixture returns false or throws, prints a per-case
`OWNERSHIP_CFG|case=...|kind=...|result=...` record, and emits one aggregate
`OWNERSHIP_DATAFLOW_V1|positive=16|negative=39|attempted=55|failed=N|result=...`
record. `native_ownership_dataflow_v1_runner_continuation` independently
checks that a deliberately failing middle case does not short-circuit the
third case. New positive fixtures exercise identical-state joins, branch-local
borrow/release, drop on both branches, transfer across blocks, and all four
inclusive analysis budgets; negative fixtures assert the corresponding
overflow/invalid-graph diagnostics, bad exit placement, overlapping
mutable loans, borrow-state mismatch at joins, and self-transfer. These
fixtures remain host compiler-time tests, not source-language guest execution.

The tests are authored and registered. They are **not reported as passing** until a nonzero-step native CTest job runs on the exact PR head. They do not prove LLVM guest execution or a process-wide no-GC link closure.

## Required next work

1. Map real typed source CFG and ownership/borrow AST operations to this IR.
2. Add branch-local owner introduction and edge-sensitive phi/join proofs.
3. Model lexical scope exits, loops, `rt take`, `rt borrow` and `rt mut borrow`.
4. Add explicit unwind/defer/recover, exception, cancel and suspension exit kinds with cleanup order and continuation ownership.
5. Integrate scheduler-issued actor identity, loans, callback/FFI quiescence and supervisor restart.
6. Run sanitizer builds and cross-backend canonical fixtures before widening sealed native runtime admission.

The V1 analyzer also rejects unreachable blocks, unknown enum operation values, and non-Take operations carrying a transfer target. Take must name a distinct uninitialized destination and preserves the original acquisition sequence so cleanup order follows the actual resource lifetime rather than local declaration indexes.

No existing frontend, runtime, actor heap or codegen path was deleted. The analysis is compile-time metadata and does not allocate guest objects.
