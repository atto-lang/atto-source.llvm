# Native AtomicScalar and UniqueBox v1 — AttoLang implementation slice

**Status:** tested native C++20 allocator/Atomic/Box primitives; the **main LLVM C++17 compiler** now admits a deliberately restricted local `Atomic<int>` `.atto` AOT slice. Guest `Box<T>` and `Arc<T>` remain unsupported by the main compiler; cross-actor and Java/Rust/.NET/Wasm/Forge parity are not implemented. No background GC, OS mutex, allocator thread, or runtime malloc
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
drop. That original focused host proof did not exercise the actor scheduler or the main AOT guest compiler; separate newer compiler proof follows.

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

## 2026-11-14 milestone: main LLVM `.atto` Atomic<i64> integration

The existing `src/main/cpp/oreslang/frontend/Parser.cpp`, `Parser.hpp`,
`src/main/cpp/oreslang/types/OwnershipChecker.cpp`,
`NativeScalarTypes.cpp` and `src/main/cpp/oreslang/backend/LlvmCodegen.cpp`
now jointly admit **exactly one** stack-owned `val Atomic<int>` in
synchronous zero-argument `fnc main(): int`. The only admitted methods are
`load([#relaxed|#acquire|#seq_cst])` and
`fetch_add(int, [#relaxed|#acquire|#release|#acq_rel|#seq_cst])`
(with optional ordering arguments defaulting to sequential consistency).
The native type verifier uses a distinct noncopyable AtomicI64 category,
rather than pretending the owned storage is an i64 scalar.
The compiler rejects copies/escapes, unsupported calls, multiple owners,
invalid orders, unsupported bool/Box constructions and unverified contexts.

LLVM IR emits genuine `store atomic i64`, `load atomic i64`,
`atomicrmw add`; compiled with `clang -x ir` and **AOT executed** as
native binaries. Target lowering fails closed for Wasm32, Cortex-M,
isolated VM entrypoints and non-64-bit hosts. This is a **single-owner
main-scope primitive** — not actor-threaded atomic state, general-purpose
generics, multi-method actor concurrency or backend parity.

The exact 49-file source SHA mirror of the real C++17 LLVM main compiler
compiled and ran `tests/atto_atomic_main_v1.sh` on an independent private
GitHub Actions carrier:
https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43649826264
(including two positive exit-42 AOT fixtures and negative fail-closed
source fixtures). This is a **focused executable main-compiler slice**,
not full repository CI. The latest exact-SHA main compiler rerun also verified the explicit
Wasm32/Cortex-M fail-closed target regression and is **green**:
https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43650173266

## Actor-confined bounded Box region prototype

`src/main/cpp/oreslang/runtime/ActorBoxRegionV1.hpp` is a C++20
trusted-native wrapper around allocator-issued tickets.
Supervisor-authenticated `ActorHeapIdentity` is represented by
(runtime, actor, epoch), with one bound carrier thread, fixed slots,
generation-protected Box lifetime, synchronous checked callback access,
exact once deterministic drop and irreversible drained retirement.
The wrapper rejects foreign threads, wrong domain or epoch, slot reuse
while occupied, reentrant live callback mutation, and retirement with
live slots. Host tests pass under independent exact-SHA carrier proof:
https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43649903676
The wrapper itself also passes optimized, ASan+UBSan and TSan builds in
https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43650409937,
with independent exact-source SHA checks before compilation.

**Security boundary:** Identifiers passed into this wrapper are values,
not proof of supervisor authority by themselves. The scheduler must
authenticate the caller and certify turn/FFI/I/O quiescence externally.
The guest `.atto` compiler still needs AST-to-ownership CFG admission,
borrow lifespan enforcement, Box allocation/move/drop HIR/MIR lowering,
and complete unwind/cancellation cleanup before actor-owned Box usage
can be considered implemented. Unsupported raw pointer cross-actor
transfer remains explicitly rejected.

## Follow-up hardening audit: teardown reentrancy and intrinsic identity

An adversarial review found a **teardown reentrancy window**:
the actor Box region formerly reset owned boxes before revoking access.
A `T` destructor on the owning carrier could call back into that
same region and mint another allocation while destruction was underway.
`ActorBoxRegionV1` now synchronizes carrier binding, sets an atomic
shutdown fence **before any destructor**, and refuses all reentrant
admissions, callbacks, drops and retirement calls during teardown.
This lock is used only for carrier admission and terminal destruction,
not for per-turn actor allocation or Box access.

`FixedBoxArena::is_current_owner` now checks the actual slot state,
allocator domain, generation and allocation address independently from
the Box's metadata. The actor wrapper uses that check before exposing
a borrowed Box reference and before dropping a slot. **The caller must
still prove no concurrent lifetime mutation**; this check does not pin
an otherwise racing pointer.

The typed Box ownership-effect checker now rejects unknown enum
discriminants rather than accidentally treating a malformed compiler
event as a no-op. Main parser/type aliases also reserve
`Atomic`, `Box`, `Arc`, and `Weak` as compiler-owned type identities;
guest aliases, actor/class declarations or type-only declarations may
not spoof their intrinsic identity.

The private alternate-runner native proof builds the exact changed Git
blobs and executes reentrant-destructor, stale-generation and malformed
compiler-event regressions under optimized C++20, ASan+UBSan and TSan:
https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43651177318

The source-main compiler proof uses a *focused 53-file exact Git blob
mirror* of the C++17 toolchain source files and `.atto` regression corpus,
not a byte-for-byte copy of the entire private repository. The hardened
type-identity fixtures, true LLVM atomic IR and native exit-42 AOT proof
also passed: https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43651484998 The carrier
uses its **own** `GITHUB_TOKEN` and needs no source cross-org secrets.

This bridge is single-carrier and **does not yet implement the
multi-threaded actor method model**. Production migration across
carriers, multiple concurrent receivers, scheduler quiescence, and
guest `Box<T>` lowering remain blocked until independently proven.

### Remaining merge blockers

- Full main-frontend `Atomic<T>` support beyond the single local i64
  owner, with actor sealed state and multiple concurrent receiver methods.
- Full `Box<T>` main guest lowering into actor-owned heap, preserving
  compiler-verified borrow lifetimes and deterministic cleanup.
- Cross-backend semantic conformance (Java/Graal, Rust, .NET, Wasm, Forge),
  true actor scheduler race proof, and complete mainline integration tests.
- Branch rebase and source CI review; keep this PR **draft and unmerged**.


## 2026-11-14: scheduler-authenticated Box region (integration PR #1826)

The earlier \`ActorBoxRegionV1\` remains a single-carrier host prototype.
A separate, trusted-only \`SchedulerBoxRegionV1<T, Slots>\` now derives
authorization from the **real native scheduler's**
\`ores_native_actor_scheduler_is_current_actor\` for an exact
(scheduler domain, actor slot, generation) incarnation. It does not use
guest-supplied numeric identity or a permanently pinned OS thread.
The native scheduler guarantees at most one active turn per actor;
different turns can execute on different worker carriers.

* \`emplace_at\`, \`with_at\`, and \`drop_at\` require the exact current
  scheduler-authenticated actor turn. Foreign/root and other actor turns
  are rejected. Live borrows prohibit nested access and destruction.
* Allocation uses the existing \`FixedBoxArena\` ticket protocol and
  validates independent slot/domain/generation/address provenance.
* \`retire_after_done\` is **trusted supervisor-only**, never callable
  on a carrier. The supervisor must STOP/seal the exact actor, fence
  producers, and retain the scheduler while \`wait_done\` establishes
  quiescence (including resource loans). Only then are outstanding
  Boxes destroyed synchronously.
* A separate atomic retirement claim grants exactly **one winner**
  among concurrent supervisors; duplicate and destructor-reentrant
  attempts fail without double drop. Revocation precedes destructors.
* Destruction without a previously completed supervisor retirement is
  fail-closed (\`std::terminate\`) rather than silently destroying
  live resources while a worker may still hold a reference.

A four-worker **real C scheduler** regression exercises concurrent
mailbox producers, exact actor identity rejection, nested borrow/drop
denials, 100 × 10,000 Atomic increments (= 1,000,000), sealed STOP,
DONE-fenced teardown, and 16 competing supervisor retirement attempts.
All 24 relevant source Git blobs were mirrored exactly into a private
carrier branch, verified by SHA, then tested with optimized native code,
ASan/UBSan, and TSan:
https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43666864892

**This is not yet production guest Box support.** The C++ helper is
trusted-host-only. No \`.atto\` Box<T> parser/HIR/MIR or unsafe pointer
escape policy is silently enabled. The guest compiler must still prove
borrow lifetime, no suspension across an unpinned reference, exactly-once
drop on all return/exception/cancellation edges, and actor-state
sealing. The scheduler prototype handles externally coordinated STOP
cleanup; it does not automatically register panic/cancellation/FFI
finalizers or prove concurrent mutable methods on the same actor.
No across-actor Box transfer, Arc/Weak, unsafe untrusted allocation,
full backend parity, or hard-real-time WCET proof is claimed. Keep the
integration PR draft until these gates are independently verified.


## 2026-11-14: reconciled real LLVM compiler exact-source proof

The **actual main LLVM compiler** (not a mock/stub atomic backend) was
rebuilt from 58 byte-for-byte Git-SHA-verified source files at the
current integration PR #1826 head and tested independently at:

https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43668122170

The proof executes native AOT for both positive \`.atto\` scalar
\`Atomic<int>\` fixtures (exit code 42), checks \`atomicrmw add\`,
atomic load/store IR, and verifies the full negative admission corpus
(including reserved intrinsic identities and unsupported Wasm32 and
Cortex-M targets). This is a *focused real compiler build*, not a full
2,000+-file source repository mirror or global CTest pass.

**Toolchain discovery**: the carrier initially linked the compiler
against the LLVM 22 CMake SDK while its PATH resolved unversioned
\`llvm-config\` and \`clang\` to LLVM/Clang 21. Lifetime intrinsics
have a different IR signature in LLVM 22, so the LLVM 21 verifier
correctly rejected the new IR. The resolved workflow pins
\`llvm-22-dev\`, \`clang-22\`, \`llvm-config\` and \`llvm-as\` to
major 22, and the source fixture checks the consuming Clang's major
version before execution. The LLVM 22 \`getOrInsertDeclaration\`
and lifetime builder API compatibility fix is in this integration PR.
The successful run verifies both real LLVM compiler output and
native execution without disabling lifetime markers or IR validation.

Separately, the actor scheduler Box host adapter and exact-once
16-supervisor retirement race test passed optimized, ASan/UBSan and
TSan with 24 source Git blobs in:
https://github.com/honeypot-r-us-test/hnpt-lambdas/actions/runs/43666864892

**Merge gate remains closed**: \`Box<T>\` guest AST/HIR/MIR lowering,
deterministic compiled-path drops on exception/cancellation, full
sealed actor state integration, same-actor concurrent receivers, and
cross-backend parity have not been implemented or proven by these
host and restricted Atomic AOT fixtures.
