# LLVM shared actors and arena isolation v1

The existing native `ActorArena.h/.c` supplies a bounded host-only persistent slab and scratch bump region tied to a nonzero per-arena identity and turn epoch. Actor IDs and incarnation numbers authenticate admission, but do not themselves grant access to a sibling actor's storage. SHARED actors must receive separate actor-private arena instances, never one ambient shared heap for all SHARED actors.

The conformance regression extends the native negative tests: a slot handle allocated in one arena must abort when replayed in another arena, *even when the two host test fixtures use the same owner and incarnation*. Both arenas live in the same process and trusted execution domain, but must not share private storage. Parent/supervisor and child shared-region data is a distinct, explicit capability/lease facility, not a permissive mode of this host-only API.

**Not yet source-level complete:** allocator remains bytes/slotted host ABI; compiler-generated Oreslang class/struct allocations, actor stop/await/suspend drop glue, typed destructor order, shared-domain region leases and runtime quotas remain gating tasks. Zero tracing GC is the LLVM execution target, not a certification from this fixture. See `oreslang-common-source/docs/SHARED_ACTOR_ARENAS_V1.md`.


## Shared-domain native substrate now on this draft

`src/main/cpp/oreslang/runtime/SharedDomainArena.hpp` provides a separate bounded host-only arena for *explicitly shared* bytes, distinct from every SHARED actor's private `ActorArena`. Trusted supervisor code can issue per-actor read/write grants; each lease checks domain, actor identity **and incarnation**, rights and grant generation, and holds a C++ shared/exclusive lock until its lexical scope exits. Reusing an actor's numeric ID does not transfer a surviving grant to the new incarnation; the C++ API requires both identifiers on issuance and every read/write admission. Leases are **non-copyable and non-movable** so a moved-from lease cannot access memory after transferring its lock.

Persistent shared-region slots are generation checked, bounded and zeroed on release. A CONTROL-owned `try_retire()` first seals admission, then returns false rather than blocking if any reader/writer lease still exists; only after quiescence does it zero and retire storage. Parent actor termination does not implicitly retire a still-leased shared domain.

This is a **trusted host API**: public C++ grant structs are not cryptographic tokens and must never become a guest-facing `rt share` API as-is. Admission currently requires a trusted actor identity argument; integration must derive the already-required actor identity and incarnation from the scheduler rather than guest input, and add a supervisor-managed domain owner, real I/O/continuation quiescence, per-tenant aggregate accounting, typed destructor lowering, actor join/leave authorization and guest source fixtures.

The native test is wired into CTest and additionally executed directly against exact Git blob hashes with Clang/GCC and Clang ASan+UBSan. No full LLVM/Ores source compiler CI or untrusted sandbox claim is implied.

This branch adds a native C++ actor-ID-reuse regression and a host-only Clang/GCC + sanitizer workflow. These tests check the trusted shared-region substrate, **not** native `.atto` borrow-checker lowering, source-level actor messages, or Java/.NET parity.
