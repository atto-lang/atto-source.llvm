# Native AtomicScalar and UniqueBox v1 — AttoLang implementation slice

**Status:** usable native C++20 target runtime primitive and passing focused native tests.
This is **not yet guest AttoLang** compiler lowering. Do not claim that
`Atomic<T>`, `Box<T>`, or `Arc<T>` compile from `.atto` source as a result of
this change. No background GC, OS mutex, allocator thread, or runtime malloc
is performed by these primitives.

## AtomicScalar<T>

- Only bool and 8/16/32/64-bit integral `T` with
  `std::atomic<T>::is_always_lock_free` on the **actual native target** are accepted.
  Other types fail at compile time; no fallback mutex or torn reads.
- `load`: Relaxed / Acquire / SeqCst. `store`: Relaxed / Release / SeqCst.
  RMW: Relaxed / Acquire / Release / AcqRel / SeqCst.
- CAS failure order is explicitly limited to Relaxed / Acquire / SeqCst
  and cannot exceed its success order. An invalid ordering returns
  `InvalidOrder` **without touching the cell or expected value**.
- Compare-exchange returns `Mismatch` with updated expected on failed comparison,
  including spurious weak-CAS failure; callers must loop when appropriate.
- `fetch_add` and `fetch_sub` are explicitly wrapping atomic operations,
  distinct from AttoLang's checked scalar arithmetic. `Atomic<bool>` does not
  permit integer/bitwise fetch operations in this v1.
- The backend must require authenticated intrinsic identity before lowering;
  user-declared `Atomic` or foreign raw handles must not invoke this header.

## UniqueBox<T>

- A move-only deterministic owner; copy is disabled at C++ compile time.
- Adopts a valid pointer, trusted `noexcept` destructor, nonzero owning
  allocation domain and nonzero generation. There is **no implicit allocation**
  and no raw pointer escape API.
- `get_checked(domain,generation)` denies stale generations and foreign
  domains, but does not replace a compiler borrow proof.
- Move invalidates the source. Move-assignment destroys the old destination
  before adopting the new owner. Destruction first clears the capability,
  then invokes its destructor exactly once. Repeated reset is idempotent.
- The guest borrow checker must prevent any outstanding loan at move/drop,
  any use after take and any borrow crossing a Task suspension or actor
  teardown that can invalidate storage.
- A `Box<T>` may be moved between **threads** when `T: Send` is proven.
  Cross-actor/private-arena transfer is **not implemented**: the backend
  must relocate/safely transfer storage or serialize the payload; it must
  never send the address from an actor-private heap.

## Separate shared ownership

`Arc<T>` / `Weak<T>` remain gated. They require a domain-authorized shared
allocator, strong/weak atomics, CAS weak upgrade, overflow denial, last-strong
acquire before destruction, generation/ABA protection and physical lifetime
quiescence. Do not infer `Arc` safety from `AtomicScalar` alone.

## Proofs and unfinished work

Native exact-blob proof: the two C++ source blobs were SHA checked inside a
separate private Actions carrier before optimized compilation and ASan/UBSan
execution, with 10 concurrent lanes and 400,000 total atomic increments.
The test includes memory-order rejection, weak-CAS retries, release/acquire
publication, after-move invalidation, generation/domain denial, and exactly-once
drop. The separate actor scheduler and AOT guest compiler were **not** exercised.

Next gates before guest support:
1. Parser and typed `Atomic<T>` / `Box<T>` identity, validated target width
   and memory-order effects, source borrow/move/escape rules, and typed HIR/MIR.
2. Real LLVM native lowering to the target ABI and emitter, and parity with
   Java/Graal, Rust, .NET Native AOT, Wasm, and independent Forge contracts.
3. Actor method concurrency, allocator lifetime pinning, cancellation/STOP,
   user-supplied bad casts and FFI boundaries, cross-actor relocation or denial.
4. TSan where supported, additional target architectures and exact-SHA native
   guest execution. TSan or hard-real-time certification is **not** claimed.
