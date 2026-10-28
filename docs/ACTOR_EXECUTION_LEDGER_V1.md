# Actor-local execution ledger V1 — non-reentrant default

Status: **host-only executable contract; actor scheduler integration and guest lowering NOT implemented**.

## Purpose

A scheduler-issued wake is only *eligibility evidence*. It is not authority to run a specific guest callback. Oreslang keeps two cross-checkable but distinct records:

| VM/scheduler-owned | Actor-owned |
| --- | --- |
| Valid actor reference/incarnation, runnable reservation | Exact event sequence and slot generation |
| Physical exclusive carrier lease, domain routing | Current running or suspended logical handler |
| Priority, immutable timer admission budget, RT qualification | Await continuation state, timer callback order, cancellation cleanup |
| Bounded external wake ingress, lane capacity | Bounded actor event slots, deterministic order, lifecycle revision |

A single actor owner serializes local ledger transitions under the authenticated scheduler/actor execution lease. External timer/Future completions must pass through bounded authenticated ingress; they must NOT directly mutate the private actor ledger. The ledger's revision and event identity permit reconciliation without a distributed two-lock transaction. Repeated notifications never create extra guest turns.

## Rules

1. Default **no reentrancy across suspension**: an awaiting handler A remains the logically active handler. A later message B or newly expired callback C can be admitted and recorded but cannot enter guest code while A is suspended, even if A is not yet ready. Once A is ready, A resumes before B and C. No second physical carrier ever executes actor code concurrently.
2. A timer **await** wakes the same event/continuation token. A timer **callback** is a distinct queued event. A supervisor shutdown/cancel fence remains runtime CONTROL (not an arbitrary guest callback).
3. If the actor is suspended and no continuation is ready, do not pin an OS carrier. Scheduler may work on *other actors*. Do not treat idle-carrier availability as permission to run B within this actor.
4. A source-level opt-in reentrancy policy is **not** provided here. If one is considered, it requires a separate compiler proof of state loans, critical guards, async effects, ordering and interference. Current non-reentrant contract does not permit it.
5. Cancellation of a queued handler releases its slot; cancellation of a suspended handler schedules same-turn cleanup; cancellation while running retains its slot and physical ownership until confirmed exit. Logical STOP is not physical quiescence.
6. Opaque event token = `(actor_id, actor_generation, event_sequence, slot_generation, slot)`. Validation is exact; old generations, duplicate wakes, recycled slots and foreign tokens cannot resurrect work. Revision changes only on admitted state transitions.
7. `ActorExecutionLedger<N>` uses inline fixed-size storage, bounded O(N) admission/selection and no per-event heap allocations. **It is intentionally not thread safe.** Capacity, worst-case scan cost, and the scheduler's separate ingress/lease must be proven for a qualified hard-RT profile. Priority CONTROL and deadline misses cannot silently bypass the suspended-handler rule.
8. For hard RT, a timer callback behind an unbounded suspended/nonpreemptible handler is **not schedulable**. Admission must bound actor interference or reject the requested deadline. Measure *due → clock observation → scheduler READY → actor lease → first guest instruction*, not just `claim_under_lease`.

## Integration sequence

- Connect an actor-owned ledger to the actual `NativeActorScheduler` lane/actor arena; authenticate every event/continuation token under the same physical actor claim; provide scheduler↔actor revision/slot accounting.
- Route completion, timer expiry, cancellation and stale upgrade tokens through separately bounded ingress; ensure no lost wake while transitioning from SUSPENDED to READY.
- Preserve the existing supervisor CONTROL priority without allowing nested guest state entry; make cancellation cleanup and physical retirement an explicit barrier.
- Differential-test equivalent semantics in Java (PR #161), LLVM, .NET, WASM, Forge and Cranelift. Add a finite-state/interleaving model and a compiler-generated guest source test.
- Keep hard-RT qualification blocked until target-specific priorities, preallocation, WCET, actor-turn blocking and measured code-entry deadlines are proven.

## Evidence

`tests/native_actor_execution_ledger_test.cpp` is a **host-side** C++17 test of these transitions, not compiled Oreslang code. Until executed exact-head native CTest, runner job steps and actual guest lowering are independently verified, this design must remain draft. Do not copy private source across GitHub organizations without explicit export authorization.
