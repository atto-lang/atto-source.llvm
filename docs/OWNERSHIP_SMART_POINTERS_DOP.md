# Ownership auto traits, smart pointers, and DOP borrow contract

Status: first semantic foundation. This contract is intentionally stricter than
current backend implementation. Unsupported surface syntax or lowering must
fail closed rather than silently weakening ownership.

## Goals

Oreslang keeps Rust-style deterministic ownership while adapting concurrency
to actor-owned heaps, explicit sharing domains, and data-oriented storage.

The core proof vocabulary is:

- `Send`: ownership may move safely between concurrent execution lanes.
- `Sync`: shared borrowing may be used safely from concurrent lanes.
- `Share`: Oreslang-specific proof that backing storage may be aliased across
  actor/allocation-domain boundaries.
- `Copy`: implicit value duplication is safe.
- `Clone`: explicit potentially nontrivial duplication.

`Send`, `Sync`, and `Share` are independent. In particular, a unique
`Box<T>` can be Send without granting shared cross-actor aliases.

## Smart-pointer contract

### Box<T>

`Box<T>` is one unique owner of a heap allocation.

- Moving the Box transfers ownership without copying T.
- Drop destroys T exactly once.
- Shared/exclusive borrows dereference to `&T` / `&mut T`.
- Box does not itself imply Share.
- An actor-boundary move of unique storage requires a compiler/runtime
  allocation-domain transfer proof; raw actor-private pointers never escape.

### Arc<T>

`Arc<T>` is atomic shared ownership of storage that has explicit shared-domain
authority.

- Cloning Arc increments the strong count and never clones T.
- Normal access is immutable.
- Shared mutation requires a synchronization primitive such as Mutex/RwLock
  or atomics.
- Cross-domain Arc admission requires T's Send+Sync proof and shared backing.
- Cycles require Weak edges or an explicit traced/cycle-aware graph type.

### Rc<T>

`Rc<T>` is non-atomic shared ownership confined to one scheduler/allocation
domain. It is not Send, Sync, or Share.

### Weak<T>

`Weak<T>` never keeps the target alive. Upgrade returns an optional strong
reference. Supervisor graphs, registries, subscriptions and hot-reload
metadata should prefer Weak back-edges.

## dyn Trait

`dyn Trait` is explicit type erasure plus compiler-generated static vtable
metadata; it does not require runtime reflection.

Examples of intended forms:

    &dyn Handler
    &mut dyn Handler
    Box<dyn Handler>
    Arc<dyn Handler + Send + Sync + Share>

Auto-trait bounds survive erasure. A plain `dyn Handler` must not acquire
Send/Sync/Share merely because one concrete implementation happens to have
them.

Only dyn-compatible traits may be erased: unresolved generic methods,
unconstrained concrete Self uses, and unfixed associated types fail at compile
time. AOT builds enumerate the vtables that can actually be reached so dyn
remains compatible with closed-world tree shaking.

## Interior mutability and pinning

A shared `&T` never regains ordinary mutation. Safe shared mutation requires
compiler-recognized synchronization/interior-mutability types.

`Pin` exists for address-stable values needed by async continuation frames,
FFI, native I/O, intrusive structures and accelerator operations. Ordinary
await syntax should let the compiler insert pins where possible rather than
forcing Pin into everyday source.

## Data-oriented ownership

Ownership is independent from physical layout. A logical record/component may
be stored as AoS, SoA, or AoSoA while the checker reasons about logical places.

The canonical place hierarchy is:

    store
      -> archetype / partition
        -> component
          -> field
            -> row/chunk/lane range
              -> storage epoch

A whole-place access overlaps every descendant. Sibling fields/components are
disjoint. Disjoint half-open row ranges are disjoint. Read/read overlap is safe;
any overlap containing a write requires exclusivity.

Physical layout is never itself a permission. AoS, SoA and AoSoA use the same
alias rules and may choose different lowering/vectorization strategies.

## AoSoA and parallel chunking

AoSoA chunks are natural ownership partitions. For a lane width of 16:

    chunk 0: rows [0,16)
    chunk 1: rows [16,32)

Two exclusive borrows of those chunks are compatible. Overlapping ranges are
not.

This allows the scheduler to parallelize work without Arc or locks when it can
split one unique world/store owner into disjoint temporary chunk loans.

## ECS system access sets

Each system has a compiler-visible access set, for example:

    Movement:
      write Position
      read  Velocity
      read  DeltaTime

    Render:
      read Position
      read Sprite

Systems are concurrently schedulable iff no pair of accesses overlap with at
least one write. Runtime scheduling may use this proof, but must never invent
more permissive aliasing than the compiler contract.

## Relayout, compaction and entity identity

There is one authoritative physical layout at a time. AoS/SoA/AoSoA are not
three synchronized copies.

Relayout, compaction, swap-delete, and archetype migration require the
appropriate exclusive storage authority and advance the storage epoch. Old
physical borrows become stale.

Logical entity handles remain generation-checked identities rather than raw
component addresses, so relocation may preserve entity identity while
invalidating physical component views.

## Hot-path guidance

DOP profiles may warn when hot component columns contain indirections that
reduce locality/vectorization, including:

- Box<T>
- Arc<T>
- dyn Trait
- arbitrary graph references
- variable-width strings

These remain legal unless a stricter realtime/no-allocation profile forbids
them. The preferred ECS hot path is usually inline data plus static system
dispatch.

## Backend obligations

Java/Graal:
- reuse the existing OwnershipChecker as the source authority;
- elaborate source types into common Send/Sync/Share rules;
- keep actor-domain sharing distinct from JVM object reachability;
- use access-set proofs for ECS scheduler admission.

LLVM:
- extend typed ownership provenance beyond the current restricted native
  subset;
- represent DOP places/ranges/epochs in ownership IR before vectorization;
- preserve drop flags and loan identity through relayout/cancellation.

Forge:
- implement the same contract during bootstrap; until then ownership syntax
  must remain rejected rather than accepted without proof.

All backends must share conformance fixtures before any of these features are
reported as production-complete.
