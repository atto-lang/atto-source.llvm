# Native actor ready-index locality hardening

Status: **draft until real-thread scheduler tests execute at an exact source head**.

The live `NativeActorScheduler.c` used to scan all actor lanes under
`s->lock` for CONTROL, WAKE and DATA, in priority order. This change
maintains one fixed-capacity 1024-actor bitmap for each priority tier, so
the lookup examines at most 16 x 64-bit words per tier per circular search,
rather than up to 1024 cold `ActorLane` records for each pass. No mailbox
payloads, actor authority, or actor-owned heaps are moved.

Every bitmap mutation occurs under the same scheduler mutex as the actor
state transition. Index changes are performed when:
- new DATA is admitted;
- wake credits are published READY;
- supervisor CONTROL and typed failure are published;
- a carrier enters/leaves an actor's physical turn;
- sealed-stop abandons queued DATA.

A carrier's active actor cannot appear in any ready bitmap. Worker dispatch
still gives CONTROL first, then WAKE, then DATA. DATA's bounded same-carrier
reuse (at most two consecutive) remains; a consumed envelope stays backed by
the same reservation, incarnation, and actor-exclusive authority.

Security and failure boundaries:
- All 3 bitmaps are fixed arrays embedded in the scheduler, initialized with
  `calloc`. There are no new hot-path allocations, FFI calls, exposed
  pointers or changed actor state loans.
- `find_ready` checks the retrieved candidate's physical ownership flags
  and actual priority payload before returning; an inconsistent bitmap
  results in fail-stop rather than an unauthorized callback.
- A physical turn gate or CONTROL cancellation may delay execution, but
  indexed presence does not grant a gate permit.
- Cache line alignment, NUMA page placement and real GPU affinity are
  **not** established by an actor-ready index.
- Future actor limits above 1024 must first expand the bitmap or reject
  admission. Never index out-of-range.

## Verification

```sh
bash scripts/test-native-ready-bitmap-standalone.sh
```

This compiles the *actual in-repo header* using GCC and Clang under
ASan/UBSan, comparing 65,000 seeded mutation/lookup steps for each priority
against a naive circular scan across 13 sizes. Source-exact standalone tests
have passed locally for the three new files, verified by matching Git blob
SHA-1 values. This does **not** compile or run the full scheduler.

`.github/workflows/native-ready-bitmap-locality.yml` adds an exact-head,
no-LLVM-dependency production scheduler build and regression tests covering
FIFO, CONTROL priority, wake escrow, stop/send races and actor exclusivity.
An Actions failure with no executed steps is *not* runtime evidence.

After functional admission, benchmark against the previous exact mainline
with variable actor counts (1, 64, 512, 1024), a fixed workload, p50/p95/p99,
L1D/L2/LLC MPKI, lock contention and cache-to-cache transfers. Do not
claim hardware improvements without real pinned-host measurements.
