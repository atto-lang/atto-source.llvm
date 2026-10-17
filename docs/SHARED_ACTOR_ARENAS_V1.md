# LLVM shared actors and arena isolation v1

The existing native `ActorArena.h/.c` supplies a bounded host-only persistent slab and scratch bump region tied to a nonzero per-arena identity and turn epoch. Actor IDs and incarnation numbers authenticate admission, but do not themselves grant access to a sibling actor's storage. SHARED actors must receive separate actor-private arena instances, never one ambient shared heap for all SHARED actors.

The conformance regression extends the native negative tests: a slot handle allocated in one arena must abort when replayed in another arena, *even when the two host test fixtures use the same owner and incarnation*. Both arenas live in the same process and trusted execution domain, but must not share private storage. Parent/supervisor and child shared-region data is a distinct, explicit capability/lease facility, not a permissive mode of this host-only API.

**Not yet source-level complete:** allocator remains bytes/slotted host ABI; compiler-generated Oreslang class/struct allocations, actor stop/await/suspend drop glue, typed destructor order, shared-domain region leases and runtime quotas remain gating tasks. Zero tracing GC is the LLVM execution target, not a certification from this fixture. See `oreslang-common-source/docs/SHARED_ACTOR_ARENAS_V1.md`.


## Shared-domain native substrate now on this draft

`src/main/cpp/oreslang/runtime/SharedDomainArena.hpp` provides a separate bounded host-only arena for *explicitly shared* bytes, distinct from every SHARED actor's private `ActorArena`. Trusted supervisor code can issue per-actor read/write grants; each lease checks domain, actor identity, rights and generation, and holds a C++ shared/exclusive lock until its lexical scope exits. Leases are **non-copyable and non-movable** so a moved-from lease cannot access memory after transferring its lock.

Persistent shared-region slots are generation checked, bounded and zeroed on release. A CONTROL-owned `try_retire()` first seals admission, then returns false rather than blocking if any reader/writer lease still exists; only after quiescence does it zero and retire storage. Parent actor termination does not implicitly retire a still-leased shared domain.

This is a **trusted host API**: public C++ grant structs are not cryptographic tokens and must never become a guest-facing `rt share` API as-is. Admission currently requires a trusted actor identity argument; integration must derive identity from the scheduler rather than guest input, bind incarnation, and add a supervisor-managed domain owner, real I/O/continuation quiescence, per-tenant aggregate accounting, typed destructor lowering, actor join/leave authorization and guest source fixtures.

The native test is wired into CTest and additionally executed directly against exact Git blob hashes with Clang/GCC and Clang ASan+UBSan. No full LLVM/Ores source compiler CI or untrusted sandbox claim is implied.


## Shared-domain CONTROL and asynchronous lifetime hardening — 2026-10-17

The current shared-domain prototype no longer treats possession of a `SharedDomainArena&` as sufficient CONTROL authority. A domain issues exactly one opaque, non-copyable/non-movable `Control` capability; membership grant, revocation, and domain retirement require that exact capability. CONTROL is bound to the arena's globally non-recycling domain identity as well as its object reference, so a stale token cannot regain authority if a later arena reuses the same address. A CONTROL token from a sibling domain is rejected. This is still trusted-host authority, not guest-visible `rt share` lowering.

The domain also has bounded move-only `LifetimePin` capabilities for continuation, FFI, and I/O owners. Acquiring a pin validates a live member grant but confers no handle, slot, read, write, allocate, or drop authority. Pins do not hold the reader/writer mutex and may be released from a different completion carrier. Logical retirement seals new authority immediately, while physical scrub/reclaim remains deferred until both critical leases and lifetime pins are quiescent. Pin release is idempotent, bounded by `MaxLifetimePins`, and retirement remains retryable and idempotent.

This closes two host-substrate gaps only. Source `critical-read`/`critical-write` lowering, scheduler-derived actor identity, domain-owner construction, aggregate tenant accounting, and automatic continuation/FFI/I/O pin attachment are still required before `rt share` can be considered implemented.
