# Storage-owned state containers: access and reclamation gate (v1)

Status: **native C++ reference hardening, not source-language LLVM lowering**.
This gate applies first to `DeterministicGraphArena<T, Capacity, MaxEdges>`.
It does not claim that all LLVM guest allocations are zero-GC, that Java/Graal
runs without GC, or that actor/Task quiescence is already verified end-to-end.

## Invariants

1. **Logical ownership != extra heap.** A region is a lifetime, alias and
   authority domain; its physical backing may be stack-resident, actor-owned,
   static, explicitly allocated, or preallocated elsewhere. Neither creating a
   logical owner nor granting a read/write access requires a heap allocation.
2. **Read is immutable.** `borrow(handle)` returns a move-only read lease
   exposing `const T&` / `const T*`, not a mutable reference. A valid loan
   pins the arena; `erase` and `close` fail while any loan exists.
3. **Write is exclusive and in place.** `with_mut(handle, callback)` is a
   stack-scoped write privilege, refuses entry if there is an active read loan,
   and rejects nested arena access while the callback is running. Callback
   return type is `void`, so the API does not itself return a borrowed view.
   No copy-on-write, snapshot, per-write guard object allocation, or element
   copy is mandated. A callback that stashes an address still violates the
   intended lifetime: the source borrow checker must prohibit escapes.
4. **Unwind revokes authority.** A thrown callback restores the write gate.
   Native callbacks and destructors must not access the domain reentrantly
   during retirement. The primitive remains single-execution-lane; it is not
   a concurrent read/write lock or safe cross-carrier transport on its own.
5. **Retirement is a monotonic admission fence.** `begin_retire()`
   permanently rejects new borrows, mutations, allocations and region
   lifetime pins. An existing read lease remains valid until it is released.
   The separate move-only `hold_region()` pin keeps arena storage alive
   without granting read/write authority or preventing ordinary in-place
   mutation. `close()` returns pending while a read lease, write callback,
   construction, or region lifetime pin exists. The scheduler must retain
   and release these pins only on the serialized owner lane, after fencing
   producers and observing physical Task/FFI/I/O completion. A cancellation
   request alone is not completion or a pin-release authorization.
6. **Cycles cannot keep regions alive.** The graph links are non-owning
   generational identities. `close` drops each live node exactly once in a
   deterministic order regardless of self-links or cycles; stale handles
   cannot be reused. The owners of resources inside `T` must still implement
   appropriate nonthrowing destruction.
7. **Quiescence precedes physical destruction.** First close admission and
   fence producer generations; then ensure all admitted turns, scheduled
   callbacks, FFI operations, I/O completions, continuation resumes, device
   fences and read/write loans have physically drained. A cancellation request
   alone, or a zero refcount without producer fencing, is **not** sufficient.
   Only then close/destroy the region. Native raw pointers escaping this gate
   are unsound even if handles have generation checks.

## Backend mapping

- **LLVM:** stack/inline placement by default where provable, owned memory
  regions and deterministic bulk teardown for longer-lived data. There is no
  tracing collector in this reference arena. No general-purpose zero-GC
  Oreslang lowering is asserted.
- **Java / GraalVM:** permit inline/escape-elided values, reusable native
  storage and appropriately scoped FFM/off-heap regions. Java wrappers,
  lambdas, buffers and host runtime state may still allocate and be traced.
  Native Image does not change that fact.
- **.NET, Wasm and Forge:** enforce the same admission, alias, teardown and
  measurement semantics independently; each backend must supply its own
  executed evidence before claiming parity.

## Evidence and release gates

The `native_deterministic_graph_arena` host test checks immutable read
access types at compile time, read/write exclusion, lifetime pins and monotonic retirement admission, nested callback denial,
exception restoration, self/cross node cycles, stale-handle invalidation,
exact-once drops and the absence of C++ `operator new` / `new[]` calls for
the chosen fixed-size payload across its hot path. It does **not** observe
`malloc`, page faults, `T`-internal allocations, scheduling or guest
compiler output. This is a bounded native reference proof, not a general
zero-allocation benchmark.

Before production claims, attach **latest-head** compiler-backed guest
execution and ASan/UBSan, multiple carriers with quiescence proofs, source
borrow-escape negatives, per-operation C++/C/native allocation counters,
live bytes, teardown latency (including p99.9), cancellation acknowledgments
and fault injection. A failed runner-admission check supplies no runtime
evidence. Reports must distinguish `not executed`, `failed` and `passed`.

The primary pending integration is compiling Oreslang state-container access
into stack-only read/write authority with static nonescape and suspend checks;
the host API alone is not a substitute for that compiler proof.
