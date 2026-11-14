# LLVM hard-real-time target

Status: implementation target. The current LLVM backend is not yet certified hard real time.

The strict native profile is `native-hard-rt` and requires all of:

1. full guest AOT with no interpreter/JIT fallback;
2. no tracing collector or managed heap on the certified execution path;
3. preallocated/bounded memory after admission;
4. a schedulability proof under the selected scheduler and target;
5. bounded blocking, cancellation, cleanup, and actor-region retirement;
6. no unknown FFI or page-faulting growth after start.

The shared diagnostic categories come from `oreslang-common-source/docs/REALTIME_EXECUTION.md`.

## Current substrate

The repository already contains `OwnedHeap`, `EpochArena`, stack-only object lowering, deterministic native AOT for the admitted source subset, and scheduler/cooperation experiments.

These are useful substrates but not a hard-RT proof.

In particular, `OwnedHeap` currently uses `std::shared_ptr` and `std::mutex`. It therefore remains a general native ownership prototype and must not be treated as the strict hard-RT allocator. The hard-RT path needs fixed-capacity/preallocated region storage with bounded synchronization and bounded destruction.

## Initial compiler/runtime boundary

`HardRealtimeContract.hpp` provides a tiny fail-closed host-side admission model for backend work. It deliberately rejects:

- managed heap access;
- tracing GC;
- runtime code generation;
- unknown FFI;
- unbounded allocation/blocking/loops/cleanup;
- possible page-faulting growth;
- missing or impossible timing budgets.

This header is not source-language effect inference. The next step is to lower transitive compiler effects into this admission model and normalize diagnostics to the common contract.

## Required allocator work

The first strict allocator should be a fixed-capacity actor region:

- storage reserved, committed, and touched before RT start;
- bump or segregated fixed-size allocation with bounded execution cost;
- no system allocator in the RT window;
- generation-checked handles;
- explicit quiescence before reset;
- destructors/resources either pre-retired or included in a proven teardown bound;
- fixed-capacity mailbox/task-frame pools drawn from the actor budget.

No source operation may silently fall back to malloc/new when the RT region is exhausted. Exhaustion must be an admitted, bounded failure path.

## Scheduler work

Create a dedicated RT scheduler domain rather than reusing general work stealing.

Initial supported policies:

- fixed priority;
- EDF with admission control.

The RT domain should support preallocated workers, explicit CPU affinity/reservation policy, bounded queues, priority-inversion analysis, and target-specific interrupt/memory configuration.

The existing cooperation watchdog remains diagnostic. It is not a deadline guarantee.

Status: the dispatch substrate exists as `NativeRtDomain` (preallocated
pinned workers, static actor placement, bounded lock-free SPSC queues,
CONTROL-before-DATA, fail-closed SCHED_FIFO/affinity). Fixed-priority/EDF
admission and per-actor deadlines are not implemented yet. See
[NATIVE_RT_DOMAIN.md](NATIVE_RT_DOMAIN.md).

## AOT audit

The release gate must inspect the final linked artifact and transitive runtime/FFI dependencies, not only generated LLVM IR. It must fail if a certified path reaches a tracing runtime, interpreter/JIT, unknown allocator, or unknown blocking primitive.
