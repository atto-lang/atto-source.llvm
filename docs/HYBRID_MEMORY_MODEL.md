# LLVM / OresVM: ownership-first memory management

**Status: design contract, not a completed LLVM garbage collector or borrow checker.**

The intended Oreslang model is **Rust-style deterministic ownership**, backed by
explicit opt-in managed graph memory where necessary, with tracing GC confined
to residual host/interop or explicitly traced objects. Ownership is not a type
of tracing garbage collection: borrow analysis prevents invalid aliases,
whereas compiler-generated drop glue must actually free each last-owned value.

## Present evidence

- `src/main/cpp/oreslang/runtime/FixedArena.hpp` provides a fixed-size,
  actor-carrier-local POD-only monotonic arena: aligned bump allocation, no
  reference tracing, rejection of over-capacity allocations, and trivial
  destructors only. There is deliberately **no rewind/reset** because
  continuation and pinned I/O lease liveness are not yet proven.
- `HungryCarrier` is an experimental native C++ actor-like runtime slice,
  not native lowering of Oreslang actors, borrowing, ownership, or drops.
- `CMakeLists.txt` explicitly rejects source `rt borrow`, `rt take`,
  and related ownership forms. The backend has **no** native object allocator,
  owner-provenance typechecker, general drop elaboration, or cycle collector.
- Physical memory in the Java/Graal backend still depends on the JVM GC,
  even when source ownership is checked.
- A managed or no-GC actor profile must **not** be inferred merely from
  `require_strict_no_gc` or from the absence of an explicit collection call
  in a test. Native callbacks, FFI, and libc can still allocate and block.

## Preconditions for an actual ownership-driven zero-tracing hot path

1. Carry ownership provenance and borrow lifetimes into typed LLVM IR before
   enabling source ownership constructs.
2. Classify every allocation: stack/register, uniquely owned heap, actor
   arena, explicit cross-actor shared, linear foreign resource, opt-in
   traced graph. No silent promotion to a GC heap.
3. Compile moves and partial initialization into single-owner drop flags.
   Emit destructor/drop glue on *every* exit including panic/throw,
   cancellation, forced stop, restart, and hot-loading generation retirement.
4. Prevent a region epoch from being reset until all refs, resumable frames,
   messages, FFI/async callbacks and kernel I/O leases are quiescent.
5. Preserve ownership on failed mailbox admission, and transfer atomically on
   successful admission; protect reusable select-plan buffers from stale
   borrows and pending operations.
6. Reject or use explicit weak edges for refcounted cycles. Add an opt-in
   tracing type only after root discovery, write barriers, and collection
   isolation are implemented and tested.
7. Enforce a strict no-GC actor profile at compile time and native admission:
   no implicit tracing-heap allocations, unknown allocations, blocking
   destructors, or callbacks whose effects cannot be proven.
8. Meter live bytes, peak allocation, drop counts, GC requests, retry queues,
   and p99.9 pause latency independently for JVM and native benchmarks.

## Immediate regression requirements

Maintain CTest coverage for FixedArena alignment, capacity and no-rewind
semantics; add compiler negative tests for every ownership construct that the
LLVM lowering still cannot prove safe. Introduce sanitized lifetime/FFI stress
tests and deterministic destructor counters before enabling managed objects.

The Java reference-side bounded, lazy fallback-collector patch and detailed
contract are in
[the Java memory model](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/audit/hybrid-gc-bounded-fallback-20261010/docs/HYBRID_MEMORY_MANAGEMENT.md).

**No general-purpose zero-GC Oreslang program can be certified yet.**
