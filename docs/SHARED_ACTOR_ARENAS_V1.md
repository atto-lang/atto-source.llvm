# LLVM shared actors and arena isolation v1

The existing native `ActorArena.h/.c` supplies a bounded host-only persistent slab and scratch bump region tied to a nonzero per-arena identity and turn epoch. Actor IDs and incarnation numbers authenticate admission, but do not themselves grant access to a sibling actor's storage. SHARED actors must receive separate actor-private arena instances, never one ambient shared heap for all SHARED actors.

The conformance regression extends the native negative tests: a slot handle allocated in one arena must abort when replayed in another arena, *even when the two host test fixtures use the same owner and incarnation*. Both arenas live in the same process and trusted execution domain, but must not share private storage. Parent/supervisor and child shared-region data is a distinct, explicit capability/lease facility, not a permissive mode of this host-only API.

**Not yet source-level complete:** allocator remains bytes/slotted host ABI; compiler-generated Oreslang class/struct allocations, actor stop/await/suspend drop glue, typed destructor order, shared-domain region leases and runtime quotas remain gating tasks. Zero tracing GC is the LLVM execution target, not a certification from this fixture. See `oreslang-common-source/docs/SHARED_ACTOR_ARENAS_V1.md`.
