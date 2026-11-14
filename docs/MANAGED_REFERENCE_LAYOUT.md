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

## Actor-owned heap bridge (LLVM native runtime, October 2026)

`ActorOwnedHeap<Bytes, Slots>` now exposes:

- `compact(live_handle) -> optional<CompressedActorRef>`: only valid,
  generation-checked, representable 8-byte-aligned object starts are admitted.
- `compact_matches(original_handle, compact_ref) -> bool`: a boundary
  validation that requires the *original* full-domain/generation handle.
  It rejects foreign handles, dead objects and retired heaps without returning
  a native pointer.

This is deliberately a **runtime substrate** rather than source-language
pointer-compression lowering. Merely restoring an address from a 32-bit offset
does **not** prove that the object is live: two different actors can have the
same offset, and a recycled slot can reuse the exact same compressed bits.
An out-of-line lifetime/generation proof or statically proven borrow is
necessary. Do not infer an object's proof from a newly resolved offset; that
would admit ABA. ActorHeapIdentity/admission credentials are not themselves
a sandbox or unforgeable guest capability.

The fixed-capacity actor heap above is not a 32-GiB virtual-memory cage.
It is an existing bounded contiguous region, with an aligned base. Memory
for arbitrarily many actors must not be implemented by unconditionally
reserving a full 32-GiB cage per actor: choose bounded actor-local regions,
fallback native64 when required, or use arena tiers with explicit domain
descriptors. Compression alone does not provide hardware memory isolation.

FFI/code/stack/OS pointers, native ABI and `ActorRef<T>` remain independent
of the compact storage format. The trusted actor scheduler must supply the
active actor heap base, not a pointer loaded from untrusted actor state.
Suspend/resume on another carrier must rebind this base to the actor *heap*
identity, not the execution/continuation identity. Supervisor shutdown must
fence borrows/FFI and revoke compact dereferences before freeing the arena.

The current 8-byte quantum can cover nearly 32 GiB but requires a shift
on each decode (`base + ((stored - 1) << 3)`). A future 4-GiB byte-offset
cage permits simpler arithmetic but potentially greater virtual-address
reservation and stricter placement. Benchmark both before declaring either
faster; don't allocate a 32-GiB virtual cage for every tiny actor.



## Compiler-facing actor object fields (next native slice)

The compiler analysis layer now also has
`types/ManagedRefLayoutPlan.hpp`, a fail-closed physical-layout selector
for **already-proven logical reference kinds**. Given authenticated actor
heap and lifetime evidence, it selects 4-byte actor offsets, or an 8-byte
native64 representation when the proven arena exceeds the compressed range
or the caller deliberately requests native64. It rejects absent actor
ownership/lifetime proof, invalid alignment and unsupported 32-bit actor
lowering. Cross-actor capabilities, explicitly shared regions and FFI
pointers stay separate physical categories; opaque capabilities never
guess a native pointer width. Typed dense-row indices require their own
row-identity/generation evidence.

### Checked native LLVM lowering primitive

`backend/ManagedRefLowering.hpp` now emits a verified LLVM control-flow
fragment for a physical 32-bit `Ref<T>` field. It loads `i32`, checks
non-null, invokes a **trusted original-generation validator** bound to
the actor arena base/span and the exact stored bits, zero-extends the
encoded offset to `i64`, shifts by 3, and checks requested access size,
8-byte base alignment, bounds and 64-bit address wrap. Invalid paths
emit a real `llvm.trap`; successful paths perform the native read/write
through a non-`inbounds` byte GEP.

The validator must authenticate the original allocation generation and
current actor-heap incarnation. A guest-provided boolean or a newly
reconstructed proof from the current offset is **not sufficient**. Input
object field addresses also require compiler-authenticated source object
ownership. The result pointer is ephemeral: it must not survive `await`,
actor migration, retirement, an FFI call, or a new actor turn. Each
resume must reload the admitted actor heap descriptor.

Tests: `native_compact32_llvm_checked_load_store` verifies the actual
generated module and expected LLVM instructions; when a compatible
Clang is installed, `native_compact32_llvm_executable_e2e` compiles
that IR to a native binary and executes live read/write plus death tests
for stale-generation, retired, null, foreign arena and too-small-span
paths. These are compiler IR tests, **not** arbitrary Oreslang
source-program `Ref<T>` execution.

The source AST/HIR frontend still does not admit general `Ref<T>`
class fields. Compiler ownership/borrow proof wiring, persistent
object arena allocation, generation proof acquisition, and scheduler
resume-time base binding remain implementation gates. Never reinterpret
existing `i64` or opaque `Array<int>` handles as these fields.

This policy is a compiler-side building block, not yet wired to native
Oreslang `Ref<T>` HIR or LLVM `getelementptr`/load/store emission.
`native_compiler_managed_ref_layout_admission` tests both positive
selection and negative proof boundaries; it is included in the target-ABI
sanitizer matrix.


The native `ActorCompactFieldStore<Bytes, Nodes, LinksPerNode>` now
demonstrates real, 4-byte object-field storage with an actor-owned region.
Each object contains a conventional i64 payload and a contiguous array of
`CompressedActorRef` fields; a **separate trusted ledger** records the
*original* generational `ActorOwnedHeap::Handle` and node identity of each
linked target. Linking, unlinking, following, cycle creation, byte-offset
reuse and actor retirement have independent CTest/ASan/UBSan regressions.

This is an intentionally bounded **compiler-target/runtime proof**, not a
claim that arbitrary Oreslang classes or `Ref<T>` currently use compact
fields. The current LLVM source frontend only supports scalar i64 class
fields and opaque `Array<int>` handles. Those must remain their existing
widths. Never reinterpret an i64 value, `ActorRef<T>`, external handle,
`Future<T>`, native pointer, shared-region address or Java JVM object
as a compressed actor-local reference.

### Required compiler lowering to follow

1. Add a distinct logical managed-reference kind to typed HIR, and prove
   owner actor, lifetime, borrow/exclusive access, and locality before
   choosing a physical representation. Do not expose storage width in
   source syntax. The native host ABI and message wire layout stay unchanged.
2. Emit LLVM **i32** fields/loads/stores for admitted actor-local references.
   Keep full heap/generation proofs out of the object in a trusted ledger
   until compile-time ownership establishes the proof statically.
3. At dereference, validate the original target proof or its statically
   established lifetime; only then decode using the authenticated current
   actor heap base: `base + ((i32_ref - 1) << 3)`. Guard null, arithmetic
   overflow, bounds, lifetime, and base-domain consistency before GEP/load.
   Do not introduce unchecked `inbounds` pointer arithmetic for a forged ref.
4. Pass a stable actor-heap domain/base descriptor with the admitted actor
   turn; never infer it from a worker thread ID. Resume on a new carrier
   must reload the **same actor heap** descriptor after scheduler admission.
5. If lifetime/region proof is unavailable, fail closed or use a separately
   validated opaque handle/native64 layout. Never pair an old compressed
   offset with a newly minted generation proof: that permits ABA.
6. Include ref mode, alignment shift, and arena ABI generation in closed-world
   layout fingerprints and hot-reload migration checks. A code generation
   switch is not permission to reinterpret already-populated objects.

Because the current reference ledger stores full handles out-of-line, it
**does not establish lower total memory use or faster execution yet**; it
proves narrow physical object-field layout and safety behavior. A later
compiler pass may remove redundant ledger entries where deterministic
ownership proves all aliases remain valid. Measure total private RSS and
p99 latency against native64 at equivalent application throughput.

### Validation

`native_actor_compressed32_object_fields` is a CTest target.
`scripts/verify-target-abi-dop.sh` also compiles and runs that fixture
with ASan/UBSan using g++ and clang++ on the x86-64/ARM64 runner matrix.
Passing source repository CI, including the real compiler path, is still
required for production claims.

## Next lowering work

The header-level representation is the substrate. Compiler lowering should
next introduce a logical managed-reference type in HIR, select physical
storage after ownership/layout analysis, and emit decode operations only at
actual dereference boundaries. Common-subexpression and loop optimization
should hoist redundant base/decode work.

For ECS/DOP kernels, lowering should prefer index-preserving operations and
avoid decoding to pointers until a concrete field load/store requires an
address. This is especially important for vectorized SoA/AoSoA loops.

## Span arithmetic safety gate (October 21, 2026)

A compressed32 region descriptor must specify a nonzero base, a nonempty
contiguous region, and a span of at most `2^32 - 1` eight-byte quanta
(32 GiB minus 8 bytes). The *entire* `[base, base + size)` region must fit
the native address space without integer wrap; checking just the target
offset would admit malformed descriptors whose declared end wraps around
`UINTPTR_MAX`. `ManagedRef.hpp` now applies the full-span check on both
encode and decode, including the native64 fallback mode. Null references
remain zero, never dereferenced. Tests cover the unsigned high bit, maximum
encodable word, exact address-space boundary, and corrupt overflow spans.

This does not make offset bits alone a capability. The independently verified
actor identity, object-start, generation and turn/borrow constraints remain
mandatory at every runtime/LLVM dereference boundary.


## Measured memory cost of checked compact fields (November 2026)

The 50% RAM goal is not met by compressing the field alone. Measured with
`sizeof` for `ActorCompactFieldStore` on arm64 macOS (1,024 nodes, 4 links each):

| Per edge | Bytes |
| --- | --- |
| Raw 64-bit pointer | 8 |
| Compact field in the object | 4 |
| Out-of-line proof, `std::optional<Node>` | 48 |
| Compact field plus proof | 52 |

So an edge that needs a runtime proof costs about 6.5 times a raw pointer. Compact
fields only save memory where the ledger entry is removed, which means the borrow
checker statically proves the target's lifetime (`lifetime_proven` through a
static borrow rather than through the ledger). Reducing the proof to a smaller
generation word would also help, but must not reconstruct a proof from the offset.

Until that elision exists, do not claim RAM savings for this store. Dense 32-bit
indices with one generation per row, rather than per edge, are the cheaper option
for table-shaped data.
