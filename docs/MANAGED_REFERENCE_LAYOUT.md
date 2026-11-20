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
ownership/lifetime proof, invalid alignment, pointer widths other than 32
and 64, and (on 32-bit targets) arenas larger than the 32-bit address space.
32-bit targets select the same 4-byte ref32 word as 64-bit targets; see
"32-bit hosts" below. Cross-actor capabilities, explicitly shared regions and FFI
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

## 32-bit hosts and 32-bit heap metadata (November 2026)

Atto must run on 64-bit and 32-bit machines (i686, ARMv7, wasm32), and actor
heaps should be 32-bit everywhere to keep process memory down. That has two
halves.

### The ref32 word is the same on every host

`plan_managed_reference` now admits actor-private references on 32-bit
targets and selects the same `ActorOffset32` field (4 bytes, 8-byte quantum,
`offset/8 + 1`) as on 64-bit targets. Object layouts and the cross-backend
golden vectors therefore do not depend on host width. On a 32-bit target an
arena span above `2^32 - 1` is rejected as corrupt. native64 is reachable only
through `force_native64`, which stores the address zero-extended in 8 bytes
for layout parity with a native64 build.

`ManagedRefLowering.hpp` accepts 32- and 64-bit flat-pointer DataLayouts. The
validator ABI is unchanged on both widths: `i1 (ptr, ptr, i64 span, i32 stored)`.
All bounds arithmetic stays in `i64`. On 32-bit targets the base is a 32-bit
`ptrtoint` zero-extended for the checks. The access end must be at most
`0xFFFFFFFF` (`ref32.end.representable`, checked separately so that
`ceiling - end` can never underflow into a permissive bound), and the GEP
receives the offset truncated to `i32` only after every check has passed.
16-bit pointers and fat or capability pointers (GEP index width different
from pointer width) fail closed.

### Rule: natural size alignment, never C ABI `alignof`

i386 SysV gives `int64_t` and `double` an `alignof` of 4. Every case below
was a real failure on i686, found by running the runtime there:

| Component | Symptom on i686 | Fix |
| --- | --- | --- |
| `ActorCompactFieldStore` | objects placed at `alignof(Object) == 4`; ref32 cannot encode them, so every link was rejected | `Object` is `alignas(CompressedActorRef::alignment)` |
| `TrustedPointerWindow` | `% alignof(T)` admitted offset-4 `u64` accesses that 64-bit hosts reject | checks `% sizeof(T)` |
| `ActorShardColumns` | `dop::Layout(scalar 8, alignment 4)` throws, so no shard could be built | passes `sizeof(double)` as alignment |
| `NativeActorScheduler.c` | `size_t outstanding == UINT64_MAX` overflow guard was always false | compares against `SIZE_MAX` |

Anything a ref32 may point at must be allocated with at least
`ActorOwnedHeap::compact_alignment` (8), and every 8-byte field uses 8-byte
alignment. Compiler-lowered actor objects must take alignment from this rule,
not from the target C ABI, or i686 objects come out a different size
(`{i64, i32}` is 12 bytes in the i686 DataLayout and 16 elsewhere).

### Heap bookkeeping is 32-bit on every host

Offsets, lengths, slot indices and construction-order links inside the
bounded heaps used to be `std::size_t`, so a 64-bit host paid twice the
bookkeeping of a 32-bit one for the same capacity. They are now 32-bit
whenever the bounded capacity fits. Generations stay 64-bit: a 32-bit
generation on one hot slot would be exhausted after about 4e9 reuses, and
exhaustion must never re-admit ABA. Measured with `sizeof` on aarch64 Linux:

| Heap | Before | After |
| --- | --- | --- |
| `ActorOwnedHeap<64 KiB, 1024>` metadata | 65,664 B (64 B/slot) | 36,976 B (36 B/slot) |
| `ActorHeap<1024, 32 B>` | 65,632 B (64 B/slot) | 49,248 B (48 B/slot) |
| `ActorOwnedGraphHeap<1024, 32 B, 4>` | 114,800 B (112 B/node) | 98,416 B (96 B/node) |

The 64-bit figures now equal the i686 ones for `ActorOwnedHeap` and
`ActorHeap`. These are fixed-capacity footprints, not RSS under a workload.

### Other 32-bit hardening

* `BoundedMpscQueue` (the carrier mailbox) asserted `sizeof(size_t) == 8`, so
  no actor carrier could compile on a 32-bit host. Its counters are now always
  `uint64_t`, and `std::atomic<uint64_t>` must be lock-free at compile time
  (cmpxchg8b on i686, ldrexd/strexd on ARMv7). A libatomic lock in the
  mailbox hot path is a build error, not a silent slowdown.
* The `ActorHeap` and `ActorOwnedI64Heap` identity counters used `fetch_add`,
  which wraps through 0 and re-issues identity 1 after exhaustion. They now
  use the same CAS reservation as the other heaps. The C heap also requires
  `ATOMIC_LLONG_LOCK_FREE == 2`.

### Validation

* `tests/native_heap_pointer_width_gate.rs` builds and runs 18 heap, ref32,
  carrier and shard tests with any compiler and flag set. It first proves the
  pointer width that the flags actually produce. CI
  (`native-heap-pointer-width.yml`) runs it on x86_64, i686 (`-m32`) and
  ARMv7 (cross compiler plus `qemu-arm`). Locally it passed 18/18 in i386 and
  ARMv7 Debian containers.
* `managed_ref_llvm_lowering_test` verifies the IR for native64, i686, ARMv7
  and wasm32, and rejects 16-bit and fat-pointer layouts. With
  `--emit-target i686 --clang-arg -m32`, `managed_ref_llvm_e2e` compiles the
  i686 IR, links the unchanged death-test harness and executes it, including
  the stale, null, foreign and out-of-bounds trap paths.
