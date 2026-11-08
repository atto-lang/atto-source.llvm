# Batched native cooperation quantum — experimental host contract

This draft changes only `BasicSourceCooperationQuantum` and its host tests.
It is NOT evidence of an instrumented AttoLang guest, a safe source actor
handoff, or an enforced untrusted logical fuel quota.

## Fast and slow paths

- A committed bounded backedge increments a saturating 64-bit counter and
  tests a power-of-two mask. This inexpensive counter/mask **does occur on each
  backedge**. Monotonic-clock, watchdog and scheduler slow-path checks do not.
- Default interval stays 64 for Java parity. Verified callers may select
  128, 256 or 512; arbitrary intervals (for example 100) are rejected.
- At the 512-backedge per-dispatch fairness cap, a mandatory batched poll sets
  a sticky pending-handoff request. Later backedges remain batched rather than
  entering the slow path on every iteration.
- A compiler-inserted `pendingHandoffOnNoYieldExit()` read is **required**
  at every exit of a critical/no-yield region, after releasing all guards,
  including exceptional exits. This read is clock-free and does not yield.
- The scheduler may actually yield only with a heap-saved post-commit
  continuation and no outstanding ownership/critical loans. The caller must
  not erase a pending request without an actual legal handoff.

## Admission boundary and remaining gaps

The current compiler has **not** proven that every no-yield scope invokes the
exit hook. Until it does, callers must not select this batching mode for
security-sensitive untrusted execution. A missing exit hook is a correctness
failure: a request could otherwise be delayed until the next batch despite
the critical scope having ended.

This is separate from **hard fuel**: real untrusted tenant quotas must be
charged exactly, never replenished by cooperate/await and never inferred from
soft clock quantum. Unknown host/FFI costs, recursion and allocation paths
require independent coverage or fail-closed rejection.

## Tests and evidence

The host tests check 64/128/256/512 boundaries, 512-cap latch, no post-cap
per-edge clock polling, watchdog latching, elapsed time, overflow, and
await-versus-cooperate behavior. The same test runs as a distinct
`-O3 -DNDEBUG` CTest so optimized/release builds cannot remove assertions.
Require exact PR-head CI with nonzero **executed steps**, and separate
compiler-backed .atto guest and negative mutation receipts, before readiness.
