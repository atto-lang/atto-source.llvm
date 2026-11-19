# Native actor physical parallelism contract v1

The LLVM runtime distinguishes **global physical carrier capacity** from
**same-actor execution authority**.

## Current invariant

`NativeActorTurnCredits` can represent multiple simultaneous physical turn
credits, and `NativeActorScheduler` can run distinct actor domains on separate
pthread carriers. Neither mechanism authorizes two mutable turns of the same
actor.

The scheduler therefore retains exactly one live execution authority per actor
generation. Its ready-queue test records both:

- per-actor active/high-water counters, which must remain exactly one; and
- a scheduler-wide active/high-water counter, which must reach two when two
  distinct actors are deliberately blocked on two prestarted carriers.

This is physical overlap, not merely queued or logical concurrency.

## Source admission

The compiler-facing `@ActorMaxThreads` gate remains fail-closed:

- LOW is admitted;
- MEDIUM and HIGH are rejected with the per-turn actor-state-isolation
  diagnostic.

That rejection must remain until the compiler can prove non-overlapping mutable
actor-state authority for every simultaneously executing turn.

## Promotion requirements

Same-actor physical parallelism requires executable proof of all of the
following before MEDIUM/HIGH can lower to the native scheduler:

1. sealed actor state with no direct mutable bypass;
2. compiler-proven critical-read/critical-write or equivalent disjoint state
   authority for every simultaneous turn;
3. borrow/lifetime correctness across await, select, continuation resume and
   CONTROL delivery;
4. panic/raise/throw unwind that cannot leave duplicate live mutable owners;
5. linearizable mailbox admission, stop, retirement and generation rollover;
6. ASan/UBSan plus race-detector coverage where supported;
7. compiler-generated guest execution, not only host-side C/C++ fixtures;
8. parity with the Java backend's same-actor rejection and cross-actor overlap
   contract.

A workflow with zero executed steps is runner admission/infrastructure failure
and is not execution evidence for this contract.
