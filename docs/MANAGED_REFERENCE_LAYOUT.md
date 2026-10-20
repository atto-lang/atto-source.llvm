# Native managed references and data-oriented layout

## Decision

Native Oreslang separates logical managed references from machine/ABI pointers.

* The default managed-reference representation on 64-bit native targets is
  **actor-relative compressed32**.
* A fully supported **native64** managed-reference mode remains available for
  very large actor heaps, diagnostics, specialized HPC workloads, and cases
  where compression loses on a measured workload.
* Native ABI, FFI, code, stack, MMIO and OS pointers remain target-native.
  Selecting compressed32 never changes the platform ABI.
* `ActorRef<T>` / remote actor capabilities are not actor-heap pointers and
  are not compressed by this policy.

The CMake cache setting is:

```text
-DORES_MANAGED_REF_MODE=compressed32   # default
-DORES_MANAGED_REF_MODE=native64
```


## Compressed actor reference

The initial representation uses 8-byte alignment:

```text
stored = (byte_offset >> 3) + 1
0      = null
address = actor_arena_base + ((stored - 1) << 3)
```

The stored representation is exactly 32 bits. Reserving zero for null gives a
maximum directly addressable span of 32 GiB minus 8 bytes per arena. This is an
arena span limit, not a process-address-space limit: different actors can use
different 64-bit arena bases across a much larger process VA space.

Encoding and decoding fail closed when the base is misaligned, the address is
outside the actor arena, the offset is misaligned, or the compact range would
overflow. A compressed reference by itself is not an authorization token;
ownership, borrow, generation, and actor-lifecycle checks remain authoritative.

Do not silently widen a populated actor heap in place. compressed32 versus
native64 is a physical-layout decision made before objects using that layout
are admitted.

## Data-oriented design: do not turn every relationship into a pointer

Pointer compression is only one representation strategy. Oreslang's DOP/ECS
layers should preserve the stronger option when identity is naturally a table
row: a typed 32-bit dense index.

`DenseIndex32<Tag>` is therefore distinct from `ActorManagedRef`. It is
intended for ECS entity/component tables, sparse/dense sets, SoA columns, AoSoA
tiles, graph adjacency tables, and other data-oriented structures where an
index can be consumed directly without reconstructing an address.

The optimizer/layout planner may choose among:

| Logical relationship | Preferred physical form |
| --- | --- |
| actor-owned object graph edge | compressed32 ref, or typed dense32 local index when table-backed |
| huge actor-owned object graph | native64 managed ref |
| ECS entity/component row | typed dense32 index |
| SoA/AoSoA column relation | dense index/offset when possible |
| immutable shared region | region-relative ref/handle, separately proven |
| cross-actor identity | ActorRef/capability representation |
| FFI/OS/MMIO/code pointer | native ABI pointer |

## AoS, SoA and AoSoA

Managed-reference width must not force an AoS object model. A logical
`Ref<T>` stays a logical reference through type checking; physical layout is
selected later. This allows the compiler to:

1. keep conventional actor objects in AoS while storing graph edges as 4-byte
   actor-relative references or typed heap-local indices;
2. split homogeneous or hot fields into SoA columns;
3. tile fields as AoSoA to match SIMD/cache-line geometry;
4. represent ECS relations as typed dense indices instead of pointers;
5. keep cold metadata/generations in separate columns so hot scans do not pull
   them into cache.

For a DOP/ECS table, 4-byte references or indices are valid homogeneous scalar
fields and can therefore occupy SoA columns or AoSoA lanes directly.

## Compiler/runtime invariants

* Source types do not expose `Ref32<T>` versus `Ref64<T>`; width is a
  lowering/layout property.
* The default is compressed32 unless a target/profile explicitly overrides it.
* native64 is tested and supported, not merely a debug fallback.
* FFI conversion is explicit; compressed refs are never passed as native
  pointers.
* Cross-actor movement follows ownership/message rules and never preserves an
  actor-local offset as if it were globally meaningful.
* Weak/generational references may pair a compact location/index with separate
  generation metadata; generation safety must not be dropped merely to reach
  four bytes.
* DOD passes are allowed to replace pointer-like relationships with typed
  indices/offsets when semantics and lifetime proofs permit it.
* Layout selection must be part of ABI/cache/schema fingerprints wherever
  persisted or shared binary layouts depend on it.

## Next lowering work

The header-level representation is the substrate. Compiler lowering should
next introduce a logical managed-reference type in HIR, select physical
storage after ownership/layout analysis, and emit decode operations only at
actual dereference boundaries. Common-subexpression and loop optimization
should hoist redundant base/decode work.

For ECS/DOP kernels, lowering should prefer index-preserving operations and
avoid decoding to pointers until a concrete field load/store requires an
address. This is especially important for vectorized SoA/AoSoA loops.

## Actor-local slot arena proof (runtime, not guest lowering)

`ActorManagedSlotArena<Slots, PayloadBytes, Mode>` now keeps physically
independent, 8-byte-aligned POD storage for each actor arena. The local managed
reference field is **4 bytes** in `Compressed32` mode and **8 bytes** in
`Native64` mode. Offsets refer to real allocated arena storage, not just
synthetic arithmetic tests. The initial slot prototype uses fixed-size slots,
with padded storage stride and a separate generation/liveness table.

Trusted host handles carry arena ID and generation *outside* the stored
compressed field. Admission validates owner runtime/actor/epoch, arena ID,
a live object **start** (not an interior pointer), and the slot generation.
The public API copies bytes in/out; it does not expose raw pointers to guest
code. Freed slots are scrubbed, and stale handles fail closed on reuse.
Arena IDs and slot generations never wrap. Retirement requires a joined
carrier proof and draining its lifetime leases; a failed retirement seals
new admissions until a subsequent retirement attempt succeeds.

**Important limitations:** a bare compressed32 value is not an
authentication token. It can have identical bit patterns in two different
actor heaps. That is correct: runtime provenance, region membership and
actor access must still be checked; guest code may not synthesize, transport
or reinterpret actor-local references across actors. Slot reuse currently
protects *trusted host handles*, not arbitrary compact intra-object links:
those links need verified ownership/borrow lifetimes plus a proven
incoming-reference invalidation strategy before compiler lowering can store
them without accompanying generation metadata. The arena is single-owner/
single-flight; it does not permit unsynchronized access from foreign carrier
threads. The lifetime lease blocks final reclamation but is not memory
read/write authorization.

This is **not** general variable-size allocation, compact graph field
lowering, an actor memory sandbox, or end-to-end source-level compressed
`Ref<T>`. The general actor-owned heap allocator, compiler HIR/MIR
representation selection, raw FFI pinning, and benchmark-based mode selection
remain subsequent integration work. The pure codec permits offsets spanning
up to 32 GiB minus 8 bytes at 8-byte alignment; this does not mean Oreslang
reserves that much memory per actor.

## Actual actor-slab compact32 bridge (experimental)

`ActorArena.c` is the existing native bounded, carrier-bound actor slab. Its
new `ores_actor_arena_compact_encode/read/write` functions bind a compact
slot offset to an **authenticated actor turn** and a host-only arena domain.
The native guest field is `uint32_t` (`OresArenaCompactHandle.bits`), while
the host descriptor separately records a domain ID. A foreign actor cannot
successfully use that host descriptor against its own arena. The C API never
returns a native address or performs FFI pinning.

In a slot-aligned slab, the bits are:

```text
bits = (slot_index * slot_bytes / 8) + 1
0 = null
slot_index = (((bits - 1) * 8) / slot_bytes)
```

The decode must *also* establish a non-null reference, a valid object start,
the correct owning actor's turn/carrier, a live slot previously admitted to
compact storage, field bounds and exclusive-loan eligibility on writes.
The slot stride must be divisible by eight. Malformed offsets, stale turns,
invalid object starts and unaligned slots fail closed.

**ABA guarantee:** once a slot has ever published a bare compact offset,
dropping its object zeroizes the slot and *quarantines that slot until arena
retirement*; unlike an ordinary generation-tagged handle, this 32-bit offset
cannot encode a generation number. Unpublished slots retain ordinary
generation-checked free-list reuse. This conservative policy prevents an
old compact reference from silently becoming a reference to a newly allocated
object at the same offset. It trades high churn efficiency for correctness;
future recycling is conditional on a proven no-incoming-references/ownership
analysis or a redesigned compact slot+generation representation.

**Not yet proven:** the parser/ownership checker does not admit general
source-level managed `Ref<T>` in the LLVM backend. A future compiler cannot
materialize a host descriptor from arbitrary guest-supplied `u32`. It must
also prohibit compact storage from flowing across actor mailboxes, validate
actor epoch and borrow lifetimes on suspension, and ensure the actor base
loaded after carrier migration comes from the current actor context.

The backend-only `ManagedRefLowering.hpp` now constructs LLVM IR that
keeps the guest reference as `i32`, zero extends and shifts its offset,
checks null, arena-base presence, byte bounds, and an externally established
actor/liveness predicate before executing a non-`inbounds` GEP. This emitter
does *not* supply that predicate or integrate it with source-level AST/HIR.
A separate LLVM verifier test checks its CFG and representation. The existing
source compiler remains fail-closed for unsupported pointer operations.

Memory policy: compressed32 is an **actor-local region storage choice**; it
does not shrink native FFI/OS pointers, require 4 GiB globally contiguous
address space, or justify reserving 32 GiB per actor. Small actor heaps,
bounded allocation, and per-actor recycling/retirement remain preferable.
