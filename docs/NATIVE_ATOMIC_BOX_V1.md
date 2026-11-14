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
- Adoption now **consumes a single-use `UniqueAllocationTicket<T>`** rather
  than accepting an unrestricted raw pointer. A ticket stores the trusted
  `noexcept` destructor, nonzero owning domain and nonzero generation.
  Concurrent attempts to consume the *same* ticket permit only one winner.
  An unconsumed/rejected ticket remains the unique owner and deterministically
  drops its resource on ticket destruction. No implicit allocation or raw
  pointer escape API exists.
- **Trusted allocator invariant:** only the allocator may mint tickets, and it
  must mint **at most one ticket per live physical allocation/generation**.
  This native helper cannot prevent a faulty trusted host from intentionally
  creating *different* tickets for the same pointer. The source-language
  compiler and FFI boundary must not expose ticket minting to guest code.
- Ticket lifetime cannot race with ticket adoption. The compiler/runtime
  must prove this lifetime and exclusive source ownership separately.
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

Native exact-blob proof: seven C++ source/test blobs were SHA-checked inside
an independent private Actions carrier before optimized compilation, ASan/UBSan
and TSan execution, with 10 concurrent lanes and 400,000 total atomic increments.
The ticket-adoption regression also exercises 16 racing native attempts to
consume one allocation ticket, reentrant drop, and ownership cleanup after
rejected/invalid adoption.
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
4. Allocator-issued ticket provenance, additional target architectures,
   actor scheduler integration and exact-SHA
   native guest execution. The focused native C++ test passed TSan, but that
   is **not** a guest scheduler race proof or hard-real-time certification.
