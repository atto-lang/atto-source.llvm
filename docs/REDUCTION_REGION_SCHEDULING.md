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

## Real-time scheduling extension (draft v1)

The objective is **hard-real-time-capable execution where the complete target
platform can substantiate it**, not a blanket claim that an ordinary JVM,
general-purpose OS, or unbounded FFI meets hard deadlines. A reduction budget
is a work-accounting unit, **not** a wall-clock worst-case bound.

### Region certification

Every region carries a classification:

| Class | Meaning | Strict RT admission |
| --- | --- | --- |
| BOUNDED | Proven finite work and resource bounds | Allowed if target WCET is certified |
| BUDGETED | Compiler partitions potentially unbounded work into bounded chunks | Allowed if every chunk has certified WCET |
| UNBOUNDED | Cannot prove an upper bound or reach a guaranteed boundary | Rejected |
| BLOCKING | Depends on potentially unbounded external operation | Rejected unless isolated and bounded |

The compiler must reject any cycle in the control-flow graph that can bypass
all executable, yield-safe boundaries indefinitely. This includes recursive
cycles, loops with live conflicting state loans, and calls into unknown code.
A finite number of *static* reentrancy sites is sufficient only when every
unbounded dynamic execution path reaches one within a bounded work interval.

### Timing metadata (conceptual)

```text
RealtimeRegion {
  region_id
  classification: BOUNDED | BUDGETED | UNBOUNDED | BLOCKING
  target_triple
  max_uninterrupted_work
  wcet_bound_ns?        // only present with target-specific evidence
  stack_bound_bytes?
  allocation_bound_bytes?
  preemption_boundary_ids[]
  blocking_effects[]
  proof_or_measurement_reference?
}
ActorSchedulingPolicy {
  period_ns?
  relative_deadline_ns?
  execution_budget_ns?
  max_uninterrupted_ns?
  priority?
  criticality?
}
```

All values are optional until verified; **never synthesize a WCET from average
latency, profiling, or reduction counts**. The metadata must survive backend
optimization with a verifier ensuring that no transformation removes the
required boundaries or invalidates proof assumptions.

### Admission and runtime behavior

Strict RT admission requires target-specific WCET evidence, bounded scheduler
and context-switch overhead, capacity/schedulability analysis, bounded memory
and finalization, bounded critical-section blocking, and explicit FFI effects.
Deadline, execution budget, and maximum uninterrupted interval are distinct.
A failed admission must return a diagnostic, not silently downgrade to a
hard-real-time claim. Priorities must account for inversion; a practical first
profile uses dedicated cores, fixed-capacity queues, preallocated continuation
frames, and an RT-capable OS configuration.

`critical handle_state` is single-flight between verified boundaries, not an
uninterruptible callback-wide transaction. Mutable state loans cannot be
carried across a reentrancy boundary that admits another state mutation.
Continuation-owned mutable locals can be saved and restored. Any path that
holds a conflicting loan across an unbounded region fails strict RT admission.

### Backend guarantees

- **LLVM/native:** Candidate for strict RT certification with suitable OS,
  hardware, memory, and toolchain assumptions. Native code alone does not
  establish a hard deadline.
- **GraalVM/JVM:** Default to best-effort/soft RT; JIT, GC, safepoints and
  deoptimization need explicit bounding evidence before a strict RT claim.
- **GraalVM Native Image:** May be eligible for a separately certified profile
  after accounting for its memory management and runtime behavior.

### Mandatory tests before advertising strict RT

1. Reject an unbounded loop or recursive cycle with no bounded yield path.
2. Reject an unbounded mutable state loan and unbounded/blocking FFI.
3. Preserve scheduling boundaries through optimized GraalVM and LLVM lowering.
4. Verify deadline admission failure under oversubscribed workloads.
5. Measure worst observed interruption and scheduling latency under CPU,
   memory, cancellation, and actor-contention stress; these measurements
   **support** but do not by themselves prove WCET.
6. Verify preallocated continuations, bounded queues, cancellation cleanup,
   and no unbounded allocation on certified execution paths.

**Status:** Specification only; no RT scheduler, WCET verifier, admission
controller, or cross-backend conformance evidence is asserted by this document.
