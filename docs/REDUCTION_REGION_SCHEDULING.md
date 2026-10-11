# Reduction-region scheduling and compiler-approved reentrancy (draft v1)

**Status:** Proposed shared Java/GraalVM and LLVM compiler/runtime contract. Not a claim of working preemption.

## Guarantees

- Actor, supervisor, and root logical lanes remain single-flight: no simultaneous guest execution on one lane, regardless of carrier thread.
- `critical handle_state` is a sealed intrinsic. It permits `await`, `select`, and compiler-approved reentrancy; its callback is **not** an atomic transaction spanning suspension.
- An **atomic evaluation** is the smallest compiler-proven non-suspending state operation (for example, primitive `s.count += 1` as one protected read-modify-write). A source line or arbitrary function call is **not** inherently atomic.
- **Reduction region:** an optimizable section of guest code whose uninterrupted work is bounded, or whose control-flow is subdivided into bounded chunks. Region boundaries are the only *implicit* yield candidates.
- **Reentrancy point:** a region boundary with verified continuation state and no conflicting live state-backed loans. It can admit a different continuation of the same actor after the current continuation is saved.
- Explicit `await`, `select`, `readch`, potentially blocking `writech`, and `rt cooperate` retain their own suspension semantics.

## High-performance lowering

The compiler performs control-flow, effect, ownership, and liveness analysis *before* backend lowering. It emits a **finite set of static boundary sites** (a site may execute repeatedly) and a sidecar/IR metadata table. Finite straight-line code needs no scheduling instrumentation. Hot loops with unbounded paths must reach an executable boundary: prefer existing call/allocation/channel boundaries; otherwise use **amortized bounded loop chunks** rather than per-iteration polling. An unreachable metadata-only boundary does not provide preemption. Never claim zero overhead plus guaranteed fairness for an arbitrary infinite native loop.

Normal execution must not call the scheduler per primitive operation, read a clock per iteration, or inject a check on every backedge by default. A cheap check at a *chunk boundary* may be necessary. The optimizer may choose chunk sizes subject to a configurable maximum uninterrupted-work bound; benchmarks decide thresholds, not language semantics.

## Metadata v1 (conceptual, backend-neutral)

```text
ReductionRegion {
  id, entry, exits[], cost_model, max_uninterrupted_work,
  boundary_ids[], effect_summary
}
ReentrancyPoint {
  id, resume_target, live_values_and_locations[],
  critical_lane_id, state_loan_status, native_loan_status,
  cleanup_stack, cancellation_safe, yield_safe
}
```

IDs are stable within a compiled artifact, not across builds. The backend must preserve the meaning of metadata after optimization, including mapping resume targets and live values. An entry marked `yield_safe` requires actual resumable continuation lowering; metadata alone is insufficient.

## Borrow and mutation rules

- Continuation-owned mutable locals may survive a yield and must be saved/restored.
- A mutable or immutable **state-backed loan** cannot cross a reentrancy boundary if another continuation could invalidate its exclusivity, immutability, or lifetime. A state access *capability* may survive but must be revalidated on resumption.
- Non-escaping local borrows can survive only if their owners remain continuation-owned and their alias rules are preserved.
- A native/FFM loan may cross a boundary only with an explicitly proven pinned lifetime and no conflicting access; initial implementations should reject such crossings.
- The compiler must not silently suppress fairness indefinitely because of an unbounded borrow. Reject unbounded non-yieldable paths, or transform them into proven-safe segmented borrows.
- A nested `handle_state` on the same active logical continuation may reenter under a tracked depth/capability. Different continuations require lane admission; carrier-thread identity never grants access.

## Scheduling and cancellation

A reduction budget is charged at region boundaries, not every instruction. Once exhausted, the runtime saves the continuation at a verified boundary, releases the actor's exclusive logical lane, and requeues it fairly. On resume, revalidate state capabilities and restore locals; cancellation must run cleanup and retire loans exactly once. No JVM monitor or OS mutex may be held across suspension. Blocking or unbounded FFI must be isolated or explicitly declared; metadata cannot preempt arbitrary native calls safely.

## Required conformance gates

1. Pure finite arithmetic: no unnecessary scheduling calls.
2. Infinite primitive loop: another actor progresses within the configured work bound.
3. State compound assignment: no lost updates or mid-operation reentrancy.
4. `await` / `select` inside `critical handle_state`: interleaving but never simultaneous state access.
5. Nested same-continuation state access; queued different-continuation access.
6. Mutable continuation local survives yield; live mutable state borrow crossing yield is rejected.
7. Infinite loop with live state borrow: diagnostic or proven segmented transformation, never silent starvation.
8. Panic/cancel/finalization, carrier migration, and FFI loan fencing.
9. Benchmarks: tight loop throughput vs uninstrumented baseline, p50/p99 scheduling latency, throughput under actor contention.

**Release gate:** neither backend claims preemptive fairness until a real runaway synchronous loop is demonstrably suspended and resumed, with exact-tree CI evidence. 
