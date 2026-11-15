# Batched native cooperation quantum — experimental host contract

This draft changes only `BasicSourceCooperationQuantum` and its host tests.
It is NOT evidence of an instrumented AttoLang guest, a safe source actor
handoff, or an enforced untrusted logical fuel quota.

## Fast and slow paths

- A committed bounded backedge increments a saturating 64-bit counter and
  tests a power-of-two mask. This inexpensive counter/mask **does occur on each
  backedge**. Monotonic-clock, watchdog and scheduler slow-path checks do not.
- Default interval is main's 128-backedge boundary. Verified callers may
  select 256 or 512; finer (64) or arbitrary intervals (for example 100) are
  rejected. Each boundary issues a one-shot slow-path ticket: an eager caller
  cannot sample the clock or answer twice for the same boundary.
- At the 512-backedge per-dispatch fairness cap, a mandatory batched poll sets
  a sticky pending-handoff request. Later backedges remain batched rather than
  entering the slow path on every iteration.
- `beginDispatch()` does **not** acknowledge watchdog/fairness debt.
  An unrelated await, task resumption, or carrier migration preserves the
  pending request. Only `recordedCooperate()` acknowledges the real handoff
  and resets the old exhausted dispatch counter so it cannot relatch.
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

The native host tests check 128/256/512 boundaries, one-shot tickets, 512-cap latch,
no post-cap per-edge slow polling, watchdog latching, debt survival across an
unrelated await/dispatch reset, once-only true handoff acknowledgement,
elapsed time, overflow, and await-versus-cooperate behavior.

The opt-in **source-compiled** `.ores` scalar-frame harness also exercises
fuel 0/1/2/63/64/65/127/128/129/255/256/257/511/512 as compiled machine
code, checks cancellation/resumption without state replay, and applies
negative mutations directly to the compiler-emitted LLVM IR: raising the
512-unit cap (Linux, via the trap children) and eliding the dispatch fuel
charge into the batch endpoint. The mutated IR must
still compile as optimized native code but must fail the real guest oracle.
These tests prove bounded scalar continuation only; they do **not** imply
general `.atto` actor-loop checkpoints or `critical` lowering. The same test runs as a distinct
`-O3 -DNDEBUG` CTest so optimized/release builds cannot remove assertions.
Require exact PR-head CI with nonzero **executed steps**, and separate
compiler-backed .atto guest and negative mutation receipts, before readiness.

## Current compiler-backed scalar subset (2026-11-07 extension)

Both experimental LLVM emitter paths now derive the **512** maximum dispatch
allowance from `runtime::SourceCooperationQuantum::kMaxBackedgesPerDispatch`,
rather than maintaining unrelated compiler literals. The opt-in scalar-frame
lowering (main's batched design) charges the whole dispatch allowance **once**
into a precomputed `dispatch.batch.stop` endpoint; the hot loop has a single
source-bound branch and no per-backedge fuel predicate.
Its native ABI returns once the host-selected quantum expires (64/128/256/512).
The runtime/host can then perform its expensive scheduler/watchdog check once
per dispatch. The emitted `loop.check` and `loop.commit`
blocks have **no slow-path host calls**; a Python structural guard inspects
the real compiler-produced LLVM IR and kills a deliberately inserted
per-backedge call mutation.

The `native_rt_scalar_frame_aot_two_tasks` CTest now runs under Python
`-O`, with every admission/compilation/negative mutation check remaining
active. Its optimized Clang `-O2 -DNDEBUG` compiled-guest harness verifies
all requested dispatch boundaries, 64/128/256/512 exact quantum progress,
post-cancel no-replay, and a host-driven example where a logical-task quota
of 384 reductions is not refilled across three 128-step dispatches. The quota
in that example is **host owned**. It is NOT enforced by the experimental
24-byte guest frame: a malicious host could still supply more fuel.

Intentionally weakened generated-IR programs must independently compile
and then **fail** the native guest oracle (fuel charge -> constant 1000
endpoint everywhere; 512 cap -> 4096 on Linux).
Three further structural IR mutants must fail the hot-CFG/no-slow-call oracle.
No source-level `critical`, opaque FFI, `defer`, nested loop, general actor,
or `rt cooperate` execution is admitted by this scalar-only path.

**Executed local host smoke**: the exact
`SourceCooperationQuantum.h` Git blob `9ac9b18541e7fd6c28cc1c04aedaabfe30311ba3`
was compiled under both g++ and clang++ with
`-std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`; focused
64/128/256/512 sticky request, clock read count, cap and acknowledgement
assertions passed. This is **host-only** evidence. Full LLVM CTests and
source-compiled guest mutations have **not** executed in this environment;
GitHub Actions zero-step admission remains blocked. Do not promote the PR.
