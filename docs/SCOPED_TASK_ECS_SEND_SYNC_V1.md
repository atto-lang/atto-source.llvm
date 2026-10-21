# Scoped Tasks, Send/Sync and partitioned ECS/DOP — staged design

Status: **native host borrowing reference implemented; guest-language contract proposed** (2026-10-18).
This document does **not** claim source-level `Send`, `Sync`, `Arc`, `World.partition_mut`,
`nb` capture lowering, actor-confined parallel child Tasks, or end-to-end LLVM/Java parity.
Related: Java ECS #215, Java inverted-class #1423, LLVM inverted SoA #657, Java TaskScope #1407.

## Decisions

1. **Actors are optional.** Ordinary programs, functions, `async/await`, `nb` Tasks,
   ECS worlds, inverted classes, columnar arrays, and CPU/GPU kernels must work with
   **zero actors**. Actors can own a World or supervise computation; they are neither
   the unit of dense data nor the carrier of every computation.
2. `Send` is a compiler-derived proof that ownership can safely move between
   concurrently scheduled execution contexts. `Sync` proves shared immutable
   references may be accessed concurrently (`T: Sync` iff `Borrow<T>: Send`).
   A Task may migrate between OS workers; neither trait encodes a *specific
   physical thread's identity*. Thread-affine FFI requires additional explicit
   affinity/capability and a scheduler that upholds it.
3. `Send/Sync` **do not grant cross-actor authority**. The existing
   actor `Sendable`/domain-admission/serializer distinction must remain:
   `Send` answers task transfer, `Sync` answers concurrent aliases,
   `ActorSendable` (or a carefully versioned existing `Sendable`) answers
   actor-boundary authorization, and `Serializable` answers wire encoding.
   A private actor pointer cannot become public merely because its underlying
   element type is thread-safe.
4. A unique `BorrowMut<T>` can be transferred to a scoped child Task only
   when the parent loses access until **physical task quiescence**. A mutable
   region can be *split* into disjoint field/range privileges, with no overlapping
   writers, even inside a single column. Sharing an immutable frozen `Arc<T>`
   uses atomic lifetime counts, not mutable aliasing.
5. ECS scheduling combines **static** type/effect and capture proofs with
   **dynamic** actual range/region identity checks. Component-level conflict
   metadata alone is too coarse: two Position writers on nonoverlapping rows
   may execute concurrently. Unknown/overlapping footprints serialize or reject.
6. Structural changes (grow, shrink, migrate archetype, swap-remove, close,
   hot-reload, retire arena) cannot invalidate outstanding spans or child frames.
   Cancellation is a request, not proof of physical completion. Awaiting a
   cancelled Task's result is not necessarily the required `getDone` barrier.
   Actor handlers retain existing single-handler/turn admission rules.

## Source type rules (proposed, **not executable syntax**)

| Candidate type | `Send` | `Sync` | Actor crossing |
| --- | --- | --- | --- |
| Plain immutable POD and recursively valid fixed arrays | Auto | Auto | Separate actor admission still required |
| Uniquely owned `Vector<T>` | If `T: Send` | If `T: Sync` for immutable borrows | Not automatic |
| `Arc<T>` frozen shared allocation | If `T: Send + Sync` | If `T: Send + Sync` | Only authorized compatible shared domain |
| Exclusive `BorrowMut<T>` | If `T: Send`, scoped lifetime valid | Not shareable as mutable aliases | No escaping actor-private scopes |
| Readonly `Borrow<T>` | If `T: Sync`, scoped lifetime valid | Context-sensitive | Actor policy still applies |
| Raw pointer, actor-private capability, unsynchronized mutable cell | Reject by default | Reject by default | Reject by default |
| Explicit mutex/atomic cell | Only with proven target ABI + payload proof | Only with proven target ABI + payload proof | Not automatic |

Derive the traits recursively and conservatively, including fields hidden behind generics,
closures, trait objects, FFI and native handles. Fail closed for unknown or forged
implementations; never silently treat a `const` name as deep immutable state.
The exact API trait names must be reconciled with Java's existing `Sendable`
and `Shareable` vocabulary before publication.

## Execution model

1. A parent function or actor turn has unique ownership of a World/SoA store.
2. The compiler splits the unique mutable borrow into **nonoverlapping**
   field/range loan tokens, or gives concurrent read tokens if `Sync` permits.
3. `scope.spawn(nb { ... })` *moves* each loan to one child Task. The Task
   does not inherit arbitrary actor-state authority. The parent cannot touch
   the loaned part of the World, resize it, or use an overlapping alias.
4. The M:N executor dispatches each Task on eligible workers. Prefer cache-local
   chunk sizing, placement/affinity hints, NUMA locality and batched work over
   one Task per entity; minimize false sharing at partition boundaries.
5. Scope exit drains completed/cancelled/failed Tasks to `getDone`/quiescent
   state and discharges every lease exactly once. Only then can the parent
   resume unrestricted owner access or commit structural `Commands`.
6. The same rules hold if the parent is an actor handler. Its turn remains
   reserved during child delegation according to actor serial/concurrent
   semantics; a later mailbox handler must not regain access to borrowed state.
   No implicit unlock across `await`.

Illustrative syntax (not implemented):

```ores
// No actor required.
let mut world = World.new();
using TaskScope.scope() as scope {
    let partitions = world.partition_mut<Position>(8);
    for part in partitions {
        scope.spawn(nb { integrate(rt take part, dt); });
    }
    await scope.join_all_done(); // physical quiescence, not just cancelled results
}
world.apply_commands();

// Actors are an optional orchestration/ownership boundary, not per-entity.
```

`Arc<T>` clones may share frozen backing allocation across authorized Task
contexts with no payload copy. That is **not** the same as cloning an ECS mutable
World, adopting actor-private backing into a global domain, or moving bytes across
process/isolate/device boundaries. Explicit copy or checked ownership transfer is
required where the backing domain is incompatible.

## Implemented native reference in this branch

`src/main/cpp/oreslang/runtime/DopScopedPartitions.hpp` implements
`ScopedColumn<T>` for 1/2/4/8-byte trivial arithmetic and enum cells
(excluding bool bit packing). This is a **standalone CPU host substrate**, not
a guest `InvertedArray<T>` integration.

- `read(first,count)` and `write(first,count)` create move-only,
  runtime-checked region leases backed by the **same** vector. Two readers
  may overlap; a writer excludes overlapping readers/writers but not
  disjoint readers/writers.
- Borrow admission holds one metadata mutex and performs bounds/overflow,
  conflict, ticket-generation and strong-lifetime checks. Element loops
  directly traverse contiguous underlying columns, without a lock per
  element or cloning the column. Safe APIs are `for_each((index,value))`
  for reads and `transform((index,value)->value)` for writes; inputs
  and outputs are scalars passed by value, not buffer pointers. Zero-length partitions are conservative leases: they block
  structure changes but cannot overlap nonempty ranges.
- `get`/`set` reject conflicting outstanding range access; `resize`
  refuses **all** live loans, including empty ones. A holder dropping early
  releases only its own monotonic ticket.
- The previously public `span()` getter was **removed** after an audit found
  that callers could retain a raw span after releasing a lease and resizing
  the vector. Neither safe bulk API exposes a pointer/reference to storage.
  Runtime lease size checks also reject stale/released authority.
- A bulk callback owns an internal operation pin for its entire lifetime;
  no metadata mutex is held while transforming elements. A nested call
  on the same lease is rejected. If a callback calls its own `release()`,
  release is deferred until callback exit, including throwing callbacks:
  no other writer or structural resize is admitted halfway through.
- A callback exception leaves any **already updated scalar cells** updated;
  this is a nontransactional computational operation, not an ECS structural
  transaction. The caller must use explicit staging/rollback if atomic
  multi-cell computation is required.
- Destroying an owner rejects **new** operations, while active operations
  pin the backing allocation until callback exit. A retiring owner must not
  race another thread still invoking methods **on the owner object itself**.
- One `Lease` object must not be concurrently moved, destroyed, or manually
  released by one host thread while another host thread invokes its methods;
  operation admission detects concurrent *bulk* entry, but the C++ reference
  is not a generalized thread-safe lease-object wrapper. Oreslang's compiler
  and TaskScope must enforce exclusive handle ownership and physical joins.
- Actor domain authority, allocator quotas, scoped source borrow escape,
  TaskScope cancellation, and physical completion still require integration.

`tests/dop_scoped_partitions_test.cpp` exercises two simultaneous same-column
writers, disjoint read/write, overlapping rejection, index bounds, empty loans,
resize/structural exclusion, multiple readers, stale-lease operations, callback
reentrancy, nested bulk admission, release-in-callback, release-on-throw,
post-owner retirement, 8-worker parallel 8,192-element writes and a deterministic
4,000-trial interval admission differential oracle. No actor is involved.
A compile-time negative check also ensures neither read nor write leases expose
`.span()`.

## Security/ownership and performance gates before enabling source

- Parser/AST/typechecker traits and auto-derivation, plus negative
  transitive `!Send`, `!Sync`, privacy, raw pointer, interior mutable
  and unsafe generic/closure cases in **Java and LLVM**.
- Scoped Task captures tied to lexical loans and runtime final-child
  quiescence; reject leaked references in detached `do nb`, closures,
  return values, `await`, cancellation and exceptions.
- Wire the range privilege to actual typed `EcsWorld` / `InvertedArray`
  columns, not a parallel proprietary array abstraction. Share the owner,
  generation, column and footprint descriptor with existing SoA APIs.
- Structural commands perform *full* batch preflight and structural
  barriers; row handles remain generation-checked after swap delete.
- Actor-local arena retirement, tenant/trust-domain isolation and sharing
  permissions independently constrain otherwise `Send/Sync` types.
- Exact native source files run locally with GCC 14/Clang 17 C++20 strict
  warnings, AddressSanitizer+UndefinedBehaviorSanitizer (both compilers),
  and GCC ThreadSanitizer (passed in the available environment). Full native
  source-repository CMake/CTest, Java JDK25/GraalVM, LLVM guest source
  compile/execution, cross-backend parity and performance still require
  independent confirmation. Do not claim the above host tests
  prove source `nb`, ECS or `Arc` working.
- Benchmarks must compare dense AoS, SoA, AoSoA on both partial-column
  and complete-row workloads. Measure bytes copied, pointer identity,
  allocations, cache misses, false sharing, p99 latency and scheduling
  overhead. Borrow checking does not eliminate required scheduler/Arc
  atomics or guarantee a cache-speedup.

See https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/215 ,
https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1423 ,
https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/657 ,
https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1407 .
