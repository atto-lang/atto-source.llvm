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
-D ORES_MANAGED_REF_MODE=compressed32   # default
-D ORES_MANAGED_REF_MODE=native64
```

(without the space after `-D` when invoking CMake).

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
| actor-owned object graph edge | compressed32 actor-relative ref |
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
   actor-relative references;
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
