# alloc/pool, alloc/arc and alloc/weak — source-first slice

**Status: SOURCE IMPLEMENTATION OF SERIAL CONTROL-PLANE POLICIES. Native guest execution and actual allocator-backed `Arc<T>`/`Weak<T>`/`Pool<T>` are NOT VERIFIED.**

atto-source.llvm owns its own `.atto` sources. These modules contain real checked policy transitions, not a replacement for compiler-inserted ownership, atomic memory management, or type-safe allocation.

- `pool.SlotPool` provides bounded generation-stamped slot leasing, reuse, retirement on generation exhaustion, stale/double release rejection, and a non-closing gate while any lease remains. A backend-owned payload store must enforce exclusive lease provenance, run the destructor and scrub before `release`. PoolLease is metadata, **not** a transferable security token.
- `arc.ArcLedger` models bounded strong/weak transitions and reserves the implicit weak sentinel while the last strong owner is being destroyed. `release_strong()==2` requires the trusted adapter to destroy the payload **exactly once**, then call `finish_drop`; `can_reclaim_control` remains false beforehand. This mutable ledger must not itself be shared between threads.
- `weak.WeakKey` and `may_upgrade` define domain/generation/stale-key admission and non-resurrection. The function is only a preflight; atomic increment-if-nonzero plus authenticated allocation identity must be supplied by the runtime.

## Required actual APIs (not shipped by this slice)

```atto
// TARGET ONLY; constructors, type identities and effects require spec approval.
val owned: Arc<T> = Arc.new(take! value);
val other: Arc<T> = owned.clone(); // explicit strong owner, frozen contents
val weak: Weak<T> = owned.downgrade();
val maybe: Option<Arc<T>> = weak.upgrade();
drop! other;
drop! owned; // exactly once T destructor, weak.upgrade() becomes None
// Unique owned Pool<T> must tie each checked lease to the actual payload.
```

`share!` stays the canonical explicit sharing operation; `weak!` is the proposed ownership operation. An `Arc<T>` nominal wrapper cannot silently manufacture sharing from a scoped borrow. This feature is separate from Java's actor-local mutable `Ref<T>`/`WeakRef<T>` (PR #565).

## Integration and security requirements

1. **Compiler/type checker:** `Arc<T>`, `Weak<T>` and `Pool<T>` must own opaque, provenance-checked runtime handles. Reject direct shared mutation, live borrows across drop/suspend/actor boundary, use-after-move, unproved freeze, fake generation/foreign actor/isolate, and unsupported backend operations without emitting guest code.
2. **Runtime:** checked atomic strong/weak updates with explicit acquire/release memory ordering; no strong resurrection after zero; exactly-once drop on normal, exception, panic and cancellation exits; weak control remains allocated until final weak retirement; reject counter overflow.
3. **Pooling:** memory reuse only after destruction, zeroization where required, loan and continuation quiescence, generation advance; do not recycle exhausted generations; pool lease and allocation domain must be authenticated independently of numerical slot receipts.
4. **Cycles:** strong cycles need explicit weak back-edges, cycle prevention, or a separate validated reclamation strategy. Reference counting alone does not collect cycles.
5. **Evidence:** exact source SHA and compiler SHA; real source-to-target positive and negative guest tests; no artifact for invalid programs; JDK25/Graal AOT, LLVM Clang/GCC ASan/UBSan, .NET NativeAOT, browser/Wasm/isolate as available. Count source checks separately and mark unrun as NOT_EXECUTED.

## Test matrix (pending actual guest wiring)

- Pool: capacity=1/full, release/reuse new generation, stale/double return, reject close with live leases, end-generation retires slot, closed refuses acquisition.
- Arc: two strong/one weak, clone overflow, last-strong pending destructor, no upgrade during or after last drop, finish_drop idempotence, implicit weak sentinel, final weak reclamation.
- Weak: matching generation upgrades only while strong>0, cross-domain/slot mismatch denial, stale/recycled generation denial, generation exhaustion, no forged-key authority.
- Concurrency: CAS/upgrade versus last drop races; count underflow/overflow; cancellation/unwind; no actor-local or cross-worker sharing without explicit domain proofs.

**Verification classification for this change:** `.atto` source authored; real backend compile/guest execution = **NOT_EXECUTED**. Do not mark Ready based only on source review or a host-side transition oracle.


## LLVM/native atomic Arc/Weak control integration (2026-11-22)

`src/main/cpp/oreslang/runtime/ArcControlV1.{h,c}` implements a **trusted
native runtime ABI**, not a public Atto pointer:
- C11 `_Atomic` strong/weak counters with checked 1,048,576-owner bound;
- atomic increment-if-nonzero, acquire/release CAS, and no strong resurrection;
- one implicit weak sentinel while the last-strong destructor runs;
- exactly-once destructor invocation on the 1→0 strong transition;
- allocator-provided final weak reclaimer after the payload destructor;
- immutable domain and generation checks for all public ABI transitions;
- **no hidden `malloc`, `free`, OS thread, GC or background collector**
  inside the runtime primitive (owner supplies storage and callbacks).

The host fixture `tests/native_arc_control_v1_test.c` covers invalid
generation/domain, wrong-owner release, final-drop versus weak-upgrade
races, a delayed destructor barrier, 100 × eight competing upgraders,
and physical payload/control frees through **owner-supplied** callbacks.
`tests/run_native_arc_control_v1.sh` executed locally under Clang/GCC
at O0/O2 with ASan+UBSan: **4/4 native host executions PASSED**. A separate
isolated CMake+CTest registration test also passed **1/1** locally;
the real repository's full CMake and compiler AOT build have not executed.

The LLVM compiler's existing pinned source embed generator and
`CoreLibrarySources.hpp` now register and reserve the three actual
`.atto` sources as `atto.alloc/{pool,arc,weak}` and `alloc/*` aliases
under one immutable unit identity. Generated `.inc` inputs reproduce the
source bytes exactly; the three SHA-pinned source / embed / registry
checks passed **12/12**. A native `.atto` guest fixture exists at
`tests/fixtures/alloc-pool-arc-weak-policy.atto`, but actual parse/link/
execute through `oreslang-llvmc`, Rust generator `cargo --check`, compiler
borrow/drop lowering, and AOT/sanitizer guest tests remain **NOT EXECUTED**.

**Safety boundary:** the C ABI is for compiler-proven linear strong/weak
holders and allocator-authenticated control-block pointers. The domain and
generation numbers are not authentication tokens. Calling it with a forged
pointer, duplicate raw C handle, or recycling a control block before the
last external weak retires is forbidden; the compiler/allocator must enforce
that provenance. Destructors/reclaimers must not throw or re-enter.
No generic `Arc<T>` guest API, deep freeze, actor transport or
cross-worker sharing is certified here.
