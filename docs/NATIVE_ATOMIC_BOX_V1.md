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

## UniqueBox<T> and FixedBoxArena<T, Slots>

- \`UniqueAllocationTicket<T>\` can **no longer be constructed from a raw pointer**.
  Only the trusted \`FixedBoxArena<T, Slots>\` allocator may mint a ticket.
  The source compiler does not expose ticket construction to guest code.
- The allocator owns a compile-time-bounded array of real, aligned object
  slots. Ticket issuance performs an atomic **free -> constructing -> live**
  transition. An active allocation cannot receive a second ticket; competing
  issuers admit at most one owner. No malloc/new heap fallback or GC.
- A single allocator lifecycle atomic combines **closure** and the number of
  outstanding allocation leases. \`try_close()\` is linearizable with concurrent
  \`issue_at()\`; a reported successful close cannot race the publication
  of a live ticket. Close is irreversible.
- \`UniqueBox<T>::adopt(ticket)\` consumes a single-use ticket with CAS. A
  rejected/unconsumed ticket owns its allocation and drops it exactly once.
  Move invalidates the previous box, while reset first revokes the capability
  and then invokes trusted allocator drop glue.
- Reclamation increments a 32-bit per-slot generation; generation overflow
  retires the slot forever instead of allowing stale-reference ABA revival.
  Actor/arena domain identities are process-unique (fail closed on rollover).
- The arena **must outlive** its outstanding owners; destroying an arena with
  live tickets/boxes terminates rather than leaving dangling host pointers.
  A guest compiler must still reject moves/drops with outstanding borrows,
  use-after-move and cross-actor private-heap sharing. Host-provided
  \`get_checked(domain,generation)\` is **not** a security or borrow proof.

### Proofs and limitations

Exact-source C++ tests exercise stale generations, duplicate adoption,
concurrent allocation attempts, 16-thread ticket consumption, 64 repeated
issue-vs-close races, reentrant destruction and deterministic cleanup.
The issue/close admission shares a single atomic lifecycle word; an issuer
cannot claim a slot while a successful terminal close is observable.
They run under optimized C++20, ASan, UBSan and TSan on a private runner.

This is a bounded native **foundation**, not yet wired to actor-owned arena
relocation or the main \`.atto\` borrow checker. The public native allocator
API must only be reachable through trusted backend/runtime calls.

## Experimental real guest-source Atomic AOT subset

\`src/main/cpp/oreslang/frontend/NativeAtomicAotV1.cpp\` is a **separate**,
closed-world compiler with its own typed lexer/parser. It accepts exactly one
\`val Atomic<int>\` or \`val Atomic<bool>\` local in a zero-argument
\`fnc main(): int\`, plus a small bounded collection of verified atomic calls.
It emits actual LLVM \`load atomic\`, \`store atomic\`, \`atomicrmw\` and
\`cmpxchg\` instructions (not non-atomic scalar stand-ins), and \`clang -x ir\`
produces a native executable with no JIT or hidden GC.

Memory-order validation shares \`NativeAtomicBoxV1.hpp\` rules. The separate
\`tests/atto_atomic_aot_v1.sh\` compiles positive \`.atto\` fixtures and
requires native exit code 42, while rejecting unsupported actor/Box syntax,
copy-like aliases, invalid orders, wrong payloads and unsupported operations
*before* any output LLVM IR is produced.

It does **not** provide full main compiler integration, ownership or actor
scheduler lowering, multiple atomic owners, cross-function transfers, arbitrary
expressions, or five-backend conformance. This slice must remain isolated
until those requirements are proven.

## Box compiler ownership-admission prototype

\`src/main/cpp/oreslang/types/NativeBoxCompilerAdmissionV1.hpp\` is a
**typed frontend-effect checker** wired to the existing native ownership CFG
and exact-once drop planner. It accepts only a *trusted compiler-proven*
\`Box<T>\` intrinsic identity, tracking initializes, reads, shared/exclusive
loans and loan releases, \`rt take\` into a separate uninitialized owner,
drops, and \`T: Send\`-proven one-way moves to independent threads.

It refuses copying, using an owner after move/drop, moving/dropping under
live loans, suspension with live boxed ownership (until pinned-frame
lowering exists), and cross-actor sends (until heap relocation is proven).
The plan is bounded to 64 owners and 4096 effects.

This is **not yet invoked by the main .atto parser** and does not itself
emit Box guest code. The formal admission API cannot be reached through
untrusted source user annotations. Its native tests prove the checker
contracts; integration must add proven AST-to-effect mappings and real
destructor/cleanup emission into every return/unwind/cancel path.

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
