# Actor-independent zero-copy ownership, ECS and data-oriented tasks — contract v1

Status (2026-10-18): **proposed Oreslang language contract**. This document does NOT claim working guest-language `Arc<T>`, `Weak<T>`, ECS queries, structured `nb` tasks, or actor-free source lowering in Java/LLVM. Existing Java ECS PR #215 is a compiler/runtime prototype; Java inverted-class PR #1423 is a syntax contract; LLVM inverted-class PR #657 includes a native C++ SoA reference. The C++ `DopSharedImmutable.hpp` / `dop_shared_immutable_test.cpp` on LLVM #657 provide an actor-independent *host* immutable-buffer reference, not a compiler feature.

## Primary architecture decision

Oreslang is a general-purpose systems language, **not primarily a game/simulation-engine language**. Game/simulation frameworks are useful adversarial performance and safety workloads, not the foundation of the language model.

**Actors are NEVER a prerequisite for ECS, DOP, shared buffers, SIMD, numerical kernels, ordinary concurrency, `async/await`, or `nb { ... }` Tasks.** A program with zero actor declarations can construct, own, query, mutate, and reclaim a world. Actors may later host a world, supervise workers, handle networking, partition tenants, or isolate failure domains without changing the ECS data ABI.

Keep three orthogonal concepts:
- **Storage:** ordinary AoS records, nominal inverted SoA rows, ECS archetypes/chunks, typed CPU/device regions, immutable shared buffers.
- **Execution:** synchronous functions, independently schedulable `nb` Tasks, `async/await` and channels, optional actors.
- **Authority/lifetime:** unique owners, scoped read/write borrows, immutable refcounted sharing, explicit synchronized mutable-sharing capabilities, actor/domain policies.

Do not conflate a `Task<T>` (a scheduled computation) with an `ActorRef<T>` (mailbox/lifecycle authority). A Task is not an actor and gets no implicit actor heap, mailbox, supervisor, or one-actor-at-a-time state gate.

## Library placement and public vocabulary (proposed; syntax not yet executable)

- `core/*`: non-allocating `Borrow<T>`, `BorrowMut<T>`, fixed `Span<T>`, `Sendable`/`Shareable` trait contracts and atomics *only where the target supports them*. No heap, OS, or actor required.
- `alloc/*`: `Arc<T>`, `Weak<T>`, `Vector<T>`, `Region<T>`, `InvertedArray<T>`, heap-backed ECS columns and allocator adapters. `Arc<T>` needs an injected allocator and supported atomic refcount implementation; must reject unsupported freestanding targets at compile time.
- `ecs/*` or equivalent opt-in alloc library: `World`, `Entity`, `Query`, `Commands`, `Component`, `System`, and archetype storage. No dependency on `actor/*` or `std/*`.
- `async/*`: Task/executor/scope contracts independent of `actor/*`. Actor libraries may depend on async, not vice versa.
- `actor/*`: messaging, supervision, actor-memory domains and optional host ownership of worlds; this layer must not silently redefine core ECS/Arc semantics.
- `std/*`: OS/network/clock services only where required by the selected target profile.

`rt share` remains the explicit authority boundary for share-domain publication. It is NOT an unrestricted conversion from private actor pointers to cross-actor mutable storage. `Arc<T>` is a *lifetime and immutable-alias mechanism*, not a transport permit and not a lock.

## Arc-equivalent ownership rules

1. `Arc<T>` owns one immutable allocation with atomic strong count, `Weak<T>` retains a nonowning weak count. Cloning an Arc copies a control handle and increments the count, **not T or its backing elements**. Dropping the final strong owner destroys T exactly once; a concurrent weak upgrade succeeds only while a strong owner exists. Count saturation/overflow must fail safely, never wrap.
2. Once published, the referent cannot be written through `Arc<T>`. `Arc<MutableThing>` does not bypass the checker; mutation requires a *separate* explicitly synchronized interior-mutability type and authorization. A `const` binding alone is not the proof of deep immutability.
3. Only transitively immutable, owned, actor/domain-admissible data may become Arc-shareable. Reject private pointers, callbacks, foreign resource handles, unverified aliasing, interior-mutable cycles, untrusted cross-isolate pointers, and unsound `Sendable`/`Shareable` implementations. Optional `Arc<SyncCell<T>>` or `Arc<Mutex<T>>` is a separate future synchronized API, not implicit `Arc<T>` behavior.
4. `Arc.new(value)` may allocate/relocate the value. `Arc.adopt(rt take unique)` can provide **zero element copies only when the backing allocation and allocator/domain are compatible**; an otherwise incompatible adopt must fail or use an explicitly named copy operation, never silently copy under a zero-copy claim. `Arc.clone()`, pinned immutable slices and compatible scoped borrows are always zero-payload-copy operations.
5. An `Arc` can outlive the task/actor that created it **only if** the allocation was placed in a properly owned shared heap/domain with lifetime extending to the final strong reference. Actor-owned arenas must not be freed while live public Arc handles, loans, I/O pins or device fences exist. Promotion from an actor-private arena is an explicit checked move/relocation, not an unchecked pointer cast.
6. `Weak.upgrade()` returns `Option<Arc<T>>`; cycles of strong Arc references are not collected by refcounting. Use `Weak` edges or a separately declared traced/cycle-aware owner. Destruction/cancellation/exception/teardown must each drop exactly once.
7. A pinned `SharedSlice<T>` retains a strong owner; plain `Span<T>`/pointer is a nonescaping borrow tied to the pin. Bounds arithmetic must be checked without overflow; teardown, hot-reload and task cancellation must not permit use-after-free.

The LLVM C++ `SharedImmutableArray<T>` models conservative fixed-width scalar/enum immutable arrays, slice pinning and `Weak`; it does *not* prove the general Arc heap layout, arbitrary T destructor glue, actor authorization, or source-level view escape prevention.

## ECS / inverted classes / data-oriented design

- `define component` denotes an ECS data-only POD schema. `define inverted class` denotes a nominal row API backed by independent field columns. They are separate surface abstractions and may share **lower-level** typed SoA storage only when ABI, ownership, field widths, alignment and layout metadata are equivalent.
- `World` owns stable generation-checked entity identities; chunks/archetypes use exact component signatures and bounded typed columns; `InvertedArray<T>` uses checked row handles. A physical column span is a borrowed view, never a reusable pointer-stable entity identity.
- `@reads`, `@writes`, `@optional`, `@without` and future region footprints are static access *claims*. The compiler must verify the body, including nested calls, aliasing, FFI and closures; annotations alone do not authorize a race. Disjoint columns or proven disjoint row partitions may be written in parallel; reads on the same data may coexist; conflicting writes serialize or reject.
- Iterating a dense column/query must not allocate one per-entity object or copy the entire column. Explicit `snapshot`, `to_aos`, `from_aos`, layout migration, `Arc.copy` and remote serialization may allocate/copy and must declare that effect.
- ECS structural mutation (spawn, despawn, add/remove component, change archetype, resize/reallocate) requires a barrier that quiesces all borrowed partitions/columns. Deferred `Commands` must preflight validation and ownership before commit, repair moved-entity locations, and invalidate stale query handles.
- Column buffers can be used **outside** ECS as ordinary DOP typed arrays. An ECS query can feed a normal function or a Task; it must not demand actor context. The scheduler uses field/region effects and nonescaping captures to gate concurrency.
- A shared immutable ECS *snapshot* may use Arc-backed chunks and copy-on-write on mutation; this is **not** the same as sharing a live mutable world. Copy-on-write must account for page/chunk copies explicitly.

## Scheduling and async safety (no actor requirement)

Illustrative (not yet valid/executable source syntax):

```ores
// Ordinary task scope — NO actor or ActorRef.
let mut world = World.with_capacity(100000);
world.spawn(Position{x: 0.0, y: 0.0}, Velocity{x: 1.0, y: 2.0});

let snapshot: Arc<ReadonlyChunk<Position>> = world.snapshot_chunk<Position>();
let a: Task<int> = nb { return summarize(snapshot.clone()); };
let b: Task<int> = nb { return validate(snapshot.clone()); };
let total = (await a) + (await b);

// Parallel *writes* require statically disjoint mutable partitions, not Arc.
let (left, right) = world.partition_mut<Position>(0.5);
let work_a = nb { integrate(left); };
let work_b = nb { integrate(right); };
await work_a; await work_b;
// An explicit structural barrier follows completed task scopes.
world.apply(commands);
```

- `nb { ... }` returns a `Task<T>` handle; `do nb { ... }` is the explicit fire-and-continue form. Ordinary synchronous functions still work; `async/await` is needed for waiting, not as an actor substitute.
- Task captures of borrowed data must be scoped: the owner cannot drop, reallocate, move, structurally mutate or exit scope before all borrowers finish/cancel *and drain*. A detached `do nb` must capture *owned / Arc-shared* values, not a non-`'static` temporary borrower.
- An exclusive mutable loan cannot cross an `await` unless a compiler-proven pin, scheduler-mediated suspension and exclusion protocol establish safe lifetime/exclusivity across resume; otherwise reject. `await` does not magically unlock actor state or return ownership.
- Parallel systems use deterministic conflict graphs and barrier ordering. Dependency edges (R/W, structural, device fences, FFI) must be complete; unknown effects serialize/reject. Carriers never block waiting on actor or world locks.
- Nested Tasks preserve cancellation/error/drop propagation; a task failure must not leave a permanent write privilege, leaked view or partially committed structural batch.

## Actor and process boundaries

| Boundary | Zero-copy policy |
| --- | --- |
| One function / same owner | Scoped `rt borrow` views, no copy |
| Two Tasks, immutable data in same allocation domain | Clone `Arc` handles, no payload copy |
| Two Tasks, writable disjoint columns/partitions | Scoped exclusive field/partition loans, no payload copy |
| Two actors, same approved shared allocation/trust domain | Immutable `rt share`/Arc handle with explicit admission and lifetime proof only |
| Actor-private arena to another actor | Move ownership on successful send if the storage policy permits; otherwise explicitly promote/copy; never leak private pointers |
| Different process / isolate / trust tenant / GPU device | Serialization, copy, or *explicitly* mapped validated shared-memory/device handle with lifetime fences; no implicit zero-copy assumption |

Shared ownership does **not** grant mutable cross-actor state, break tenant isolation, bypass capabilities, or imply that independently compiled code can interpret foreign addresses. Cross-process or device “zero-copy” requires distinct OS/device and security contracts.

## Required implementation / validation gates

1. Preserve actor-free `World`, `InvertedArray` and `Arc` dependency DAG at import/typechecking; no injected implicit actor or hosted I/O.
2. Define compiler generic `Arc<T>/Weak<T>` intrinsic ABI, deep-freeze and `Sendable`/`Shareable` proofs, controlled promotion, drop glue, refcount saturation and weak-upgrade race semantics. Fail closed for unsupported source/backends.
3. Integrate Java ECS #215's source effect planner/runtime with a **typed native** POD SoA column ABI; object-array prototype must not be advertised as zero-copy production. Integrate LLVM #657's typed inverted columns as a separate checked host reference and later LLVM source lowering.
4. Lower actor-free source `World` queries, component writes, structural commands, pinned column spans, `nb` captures and `await` through the ownership checker in both backends; cross-file component identity requires real module resolution.
5. Test ordinary function-only, task-only, actor-hosted and cross-actor workloads independently. Positive: clone pointer identity, weak upgrade, two readers, disjoint column/partition writes, stable handles, COW snapshots. Negative: mutate Arc, escape borrow/pin, use after destroy, overlap writes, write during structural migration, cross-tenant private pointer, detached borrowed capture, await-invalid loan, overflow/alloc failure, drop/cancel/hot-reload races.
6. Verify optimized native builds with GCC+Clang C++20 warnings, ASan/UBSan and relevant TSan; JDK25/GraalVM guest tests; full LLVM CMake/CTest, source compile/run parity, and release-grade allocation/latency benchmarks. Standalone C++ passing tests are **not** source execution proof.
7. Benchmark **pointer equality and allocation counts** as well as throughput: Arc clone O(1) payload, immutable view creation O(1), dense SoA traversal, query scheduling and structural mutation amortization. Do not call `Arc.new`, AoS<->SoA conversion or serialization zero-copy unless directly measured/proven.

Upstream links: [Java ECS #215](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/215), [Java inverted class #1423](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1423), [LLVM SoA #657](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/657), [Java shared domain #1180](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1180).
