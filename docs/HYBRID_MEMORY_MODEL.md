# LLVM / OresVM: ownership-first memory management

**Status: partial native implementation with runtime and compiler regression tests.
General source-language heap ownership and actors remain unimplemented.**

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
- `OwnedHeap.hpp` implements native deterministic unique-ownership RAII
  allocation with a bounded logical byte budget, checked shared/exclusive
  borrow leases, exactly-once destruction, move-only owners, exception-safe
  construction and rejection of actor-heap retirement while allocations live.
  The lease control block uses reference counting, not tracing; borrow leases
  may defer a destructor if host code violates source lifetime rules.
- `EpochArena.hpp` implements a single-carrier, fixed-capacity typed region
  with no malloc during `emplace`, bounded destructor slots, reverse-order
  destruction, epoch generations and move-only pinned references. Reset and
  close reject live leases. **It does not prove external raw pointers, FFI
  callbacks or async I/O have quiesced:** future compiler lowering must
  represent all such lifetimes with leases. A region with leaked leases
  terminates on destructor rather than permitting a use-after-free.
- Each experimental native `HungryCarrier` now creates a separate
  `OwnedHeap` instance with a configurable `actor_heap_limit_bytes` budget.
  `HungryCarrier::active_heap()` returns its current bounded byte heap only during that
  actor's handler turn; the I/O broker and supervisor see null. The carrier
  retires its heap after worker/producer quiescence. Byte allocations use
  generation-fenced handles and per-turn `live_blocks()` checks; `CarrierStats`
  reports protocol failures rather than the removed object/byte counters. This is *logical ownership isolation*,
  not an OS-protected or hostile-tenant sandbox; arbitrary native callbacks
  can still deliberately leak pointers.
- The LLVM frontend/codegen now accepts a narrowly restricted class form:
  integer fields initialized by literals, plus local `let Box x = new Box()`
  whose only permitted use is reading integer fields in an int expression.
  These are allocated with LLVM `alloca` and reclaimed on function return,
  with no managed heap roots, malloc or tracing. Unknown fields, duplicate
  field names, returned objects and unsupported moves fail compilation; supported
  call-scoped class borrows use the current ownership checker. This **does not implement nontrivial source destructors**.
- The current compiler admits bounded call-scoped `rt borrow` and checked
  `rt take` through its ownership CFG. Unsupported borrows, effects, and graph
  layouts fail before IR; this is not a general heap or cycle collector. The native RAII allocator is currently a runtime primitive
  and must not be misrepresented as source-level heap codegen.
- Physical memory in the Java/Graal backend still depends on the JVM GC,
  even when source ownership is checked.
- A managed or no-GC actor profile must **not** be inferred merely from
  `require_strict_no_gc` or from the absence of an explicit collection call
  in a test. Native callbacks, FFI, and libc can still allocate and block.

## Validation introduced in this change

CTest now includes `native_owned_heap_deterministic_drop`,
`native_actor_epoch_deterministic_drop`,
`native_actor_heap_isolation_and_drop`, `owned_stack_class`,
`native_exec_owned_stack_class`, plus rejection fixtures for class escape,
unknown fields and duplicate fields. GCC and Clang ASan/UBSan build/run of
the independent native primitives are performed on a credential-free
GitHub Actions mirror. LLVM compiler tests are separately exercised against
the source tree.

**Important limitation:** Those native tests prove the specific primitives;
they do not prove real Oreslang actor isolation, interop callback quiescence,
drop glue across source exceptions, async continuation lifetime safety, or
a no-GC entire production application.

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
