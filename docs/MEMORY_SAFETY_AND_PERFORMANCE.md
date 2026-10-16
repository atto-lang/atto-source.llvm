# Oreslang memory safety, zero-GC native runtime, and performance contract

**Status (2026-10-10): normative design targets, not a claim of completed runtime, source-level compiler support, Rust equivalence, or C++ parity.** The Java/Graal and LLVM compilers/borrow checkers remain independent. They must provide the same admitted safe-source semantics; their memory-management mechanisms need not be identical.

## Non-negotiable goals and scope

1. **LLVM executable/runtime: zero tracing garbage collection.** No mark/sweep, copying/compacting collector, GC safepoints/statepoints, hidden managed-heap fallback, or periodic collector is permitted in the *shipped native Oreslang execution path*. The compiler/build tools may use managed languages: their own memory management is outside this runtime claim. Malloc/free, RAII, stack allocation, actor arenas, and deterministic reference-counting for **acyclic** ownership are not tracing GC. This is **not** a promise of zero allocations, zero latency, or that OS page faults, scheduler stalls or destructor work cannot occur. Native FFI/plugins that load a collector into the same program must not be silently counted as zero-GC; either exclude them from the certified execution profile or report the claim as not satisfied.
2. **Java/Graal: minimize allocations and JVM GC work, especially on hot paths.** A JVM-hosted Truffle program and many Native Image configurations have runtime-managed objects; neither “GC disabled” nor “separate logical actor domain” proves no JVM GC. Seek allocation-free *steady-state guest hot loops* after warmup, and bounded, measurable GC impact elsewhere. Guest-owned resources must use explicit drop/close for correctness, never Java finalizers or JVM collection timing.
3. **Both backends: Rust-class safety in the safe Oreslang subset.** Safe source must not cause use-after-free, double free, dangling borrow, out-of-bounds access, data race, uninitialized read, or mutable aliasing violation. Ownership and lifetime safety must be proven across async/await, implicit `suspends` I/O, channel/select, actor send, cancellation, exceptions, hot reload and FFI boundaries. Unsupported cases fail closed rather than falling back to unchecked pointers or deferred GC.
4. **C++-class performance is a benchmark target, not a fact inferred from LLVM/Graal.** Measure equivalent algorithms with equivalent data layouts and semantic obligations on fixed hardware. Report both backends separately and never claim blanket speed parity from microbenchmarks.

## Default allocation and reclamation policy

| Value or resource | Default lifetime and allocation strategy | Disallowed shortcut |
| --- | --- | --- |
| Trivially copyable values | Registers/stack or inline aggregate, optimized away where possible | Heap boxing on each hot-loop iteration without measurement |
| Unique nontrivial owned object | Affine owner; deterministic drop on every admitted exit | Rely on tracing or Java finalizer for required cleanup |
| Collections/buffers | Preallocated contiguous, pooled or region/arena memory where workload warrants it | One actor or managed allocation for each ECS entity |
| Cyclic object graph | Single owner of an arena/graph; internal edges are non-owning generation-checked handles | Implicit strong ownership cycles that require tracing to reclaim |
| Shared immutable state | Frozen/sendable data, explicit lifetime/lease; no mutable alias | Moving an object while a foreign actor retains an untracked pointer |
| Shared mutable state | Explicit synchronization, safe alias rules and lifetime-scoped capability | Assuming actor mailboxes automatically prevent races in shared memory |
| Actor-owned state | Actor region/arena, allocated and reclaimed at safe lifecycle boundaries | Claiming Java logical memory slices are physically independent heaps |
| Native external resources | Explicit close/drop; idempotence where specified; cancellation-safe handoff | Waiting for host GC/finalizers to close sockets or release descriptors |

An arena may contain non-owning self/bidirectional links; teardown destroys owned nodes in a defined safe order without graph traversal for reclamation. Arena ownership must not be confused with dynamic node ownership, and generation IDs must reject stale, cross-actor and ABA-reused handles. An actor's arena cannot retire until active handlers, continuations, I/O/FFI leases, mailbox producer admissions and borrowed views have reached a proven quiescent state.

### Compiler obligations (required before broad source admission)

- Enforce moves, shared/exclusive loans, outlives relationships, Send/shareability and fully initialized states. Track *places/projections*, not only local variable names.
- Insert correct destructor/drop glue on ordinary scope exit, early return, failure/unwind, cancellation, deferred cleanup, actor termination and hot-reload retirement; prevent double-drop and partial-init leaks.
- Prove no borrowed address outlives an owner. Across `await`, `select`, `readch`, `*_blocking` implicit suspension, cross-thread wakeup and actor re-entry, require a valid pinned/stable owner and lifetime proof, or reject compilation. “Thread is suspended” is not a lifetime proof.
- Protect actor ownership boundaries. Every transferred value must be moved, deeply immutable/sendable, serialized, or explicitly shared with validated permissions. Actor message payloads and event callbacks are included.
- Make unsafe/FFI authority explicit. Validate pointer provenance, bounds, lifetimes, callbacks and allocation/free pairing; unsupported ABI cases reject, not silently run in the unsafe safe-source profile.
- Preserve these invariants across optimization. Future common HIR/MIR should model `alloc`, `drop`, `borrow`, `move`, `suspend`, `send`, `ffi`, `unsafe` and exceptional edges explicitly enough to verify both backends.

## LLVM execution profile: native without a collector

- No collector runtime, GC root tables, `llvm.gcroot`, `gc.statepoint`, `gc.relocate`, declared `gc` strategy, hidden tracing foreign heap or fallback to an interpreter with managed allocations.
- Audit **transitive** external calls and the linked executable, not just emitted guest IR. An IR regex by itself is insufficient for a zero-GC certification.
- Lower source owned structs/classes, arrays, closures, actor-local arenas, async frames and error paths to explicitly owned native storage and deterministic destruction. Use stack allocation/escape analysis when possible; no hidden heap promotion without an ownership proof.
- For cycles: prefer non-owning generational handles under arena ownership. There is **no opt-in tracing collector** in the strict LLVM profile. If an API intrinsically needs a managed traced graph, reject it in this profile or run it outside the certified native execution boundary with the limitation clearly documented.
- Enforce actor shutdown quiescence and prevent use of stale native pointers. Separate actor logical heaps do not by themselves provide an OS security sandbox.
- The current scalar/arena/actor heap experimental PRs demonstrate subsets. Full source lowering, collector-free linker/runtime audit and async/FFI/actor lifecycle integration remain required.

## Java/Graal execution profile: ownership-first, near-zero guest garbage

- Prefer primitives, specialized Truffle nodes, unboxed/flat representations, contiguous arrays and compiler escape analysis. Avoid per-tick boxing, closure allocations, ephemeral futures, temporary transport wrappers and repeated callback registration where semantics allow reuse.
- Track *guest hot-path* allocated bytes per tick/operation, JVM GC time/count/pause tails and live heap/RSS. Measure JIT warmup separately from steady state; include JVM interpreter, Graal JIT and Native Image modes separately.
- Put large actor-owned collections and simulation state in explicitly managed regions when justified, with safe validated handles, and deterministic release. Java `ByteBuffer.allocateDirect` / native-image Java heap objects do not automatically prove synchronous physical deallocation or separate GC heaps.
- Keep host JVM GC functioning for unavoidable implementation objects; do not disable it and claim success. Actor `gc()` may perform bounded domain-specific explicit cleanup but must not secretly call global `System.gc()`; independent GC pauses require independent physical managed heaps and runtime proof.
- Make a true zero-allocation fast path available for deterministic game/simulation ticks **without requiring an actor per game entity**. ECS/SoA data layout plus batched native/vectorized operations should remain expressible.

## Cross-backend evidence and release gates

**Every performance/safety claim requires an exact source revision, build configuration, workload, machine, repetitions and executed test evidence.** Never equate unallocated CI runners or rejected unsupported features with passed runtime tests.

| Gate | LLVM required evidence | Java/Graal required evidence |
| --- | --- | --- |
| No collector | IR audit, allowed-runtime ABI audit, whole linked image/transitive FFI audit, executed binary without any tracing runtime or GC calls | State explicitly that host GC can run; prove guest resources do not depend on it |
| Allocation | Allocator trace/counters, bytes and allocations per tick; no allocations in declared realtime no-alloc regions after startup | JFR/allocation profiler, bytes per tick after warmup, GC CPU and pauses |
| Ownership | Positive/negative move/borrow/drop cases; ASan/UBSan/TSan as applicable; crash/cancel/unwind/FFI/actor stress | Equivalent language-level cases plus actual Graal/JVM/AOT execution and reachability/cleanup stress |
| Concurrency | Stale-handle, ABA, lease, mailbox, cancellation and actor quiescence tests; scheduler stress | Actor carrier/continuation, shared-state and logical vs physical heap boundary tests |
| Performance | Equivalent optimized C++20 and Rust baselines, native `-O3` and controlled PGO/LTO/SIMD settings | Same workloads, optimized Graal profiles; cold startup and warmed steady state reported separately |
| Game workload | Deterministic fixed-tick 11v11/physics/ECS benchmark; 1k–100k entity scale tests with documented algorithms | Identical source workload/semantics, allocation-free tick feasibility and p99/p99.9 pause measurements |

Minimum published data: throughput or frame time p50/p95/p99/p99.9, allocations/operation, maximum and average RSS, CPU, code size and startup/warmup. Lock hardware/CPU governor, compiler versions, inputs, warmup counts, sample size, numeric precision and SIMD/parallelism flags. Baselines must do comparable work with equivalent safety/resource behavior. A numeric speed target may be adopted after reproducible baselines; **none is certified today**.

Safety release gates override speed: never introduce unchecked aliasing, missing bounds checks, bogus pointer provenance or unsafe allocator behavior merely to improve benchmark scores. For realtime mode, the caller must opt into explicit budget/admission constraints; fail closed when an operation could allocate, block, use unknown FFI, or require a collector.

## Next implementation priorities

1. **LLVM:** finish source-level objects and deterministic drop across *all* exits; connect persistent actor-owned heaps to source actors/async frames; prove quiescence; audit final native call graph/image for GC-free operation.
2. **Java/Graal:** build allocation profiles on real actor + select + `*_blocking` + game-tick workloads; eliminate the largest allocations; verify domain release/GC boundary truthfulness before claiming independent heaps.
3. **Both:** execute identical ownership/cancellation/cycle/FFI fixtures; add reproducible C++/Rust comparisons for physics/ECS, channels, message passing and AI-task workloads.
4. **Common contract:** version this specification with fixture identities and CI evidence; prevent unsupported code from silently compiling in either backend.

See the independent implementation efforts in Java/Graal [actor physical boundaries](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/main/docs/runtime/actor-physical-boundaries.md), [Java GC drafts](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/557), [LLVM native arena](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/38), [LLVM owned-value lowering](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/42) and [LLVM persistent actor heap](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/46). These are implementation references, **not** certification.
