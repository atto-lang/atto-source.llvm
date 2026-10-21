# Inverted classes and columnar DOP — stage-one contract (2026-10-18)

Status: **proposed language syntax; native reference storage implemented separately**. Neither Java/Truffle nor LLVM source compilers currently accept executable `define inverted class` programs. This is a normative staging contract and must not be cited as a complete compiler feature.

Related: [Java DOP issue #211](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/211), existing native `DopLayout.hpp` (AoS/SoA/AoSoA) and `DopKernelIR.hpp` (typed CPU + generated GPU kernels).

## Principle

Logical row API does **not** imply a row-wise physical object. `inverted class` declares a **nominal, class-like row API whose owning collection defaults to SoA**. It does not silently change `Array<Foo>` layout, allocate one JVM/OS heap object per row, provide pointer-stable physical addresses, or give automatic vectorization/GPU execution.

## Surface syntax — not yet executable

```oreslang
define inverted class Particle as
    float x;
    float y;
    float vx;
    float vy;

    move(float dt): void {
        self.x += self.vx * dt;
        self.y += self.vy * dt;
    }
end

let particles = InvertedArray<Particle>.with_capacity(100000);
let h = particles.push(Particle { x: 1.0, y: 2.0, vx: 3.0, vy: 4.0 });
particles[h].move(0.016);
let xs = rt mut borrow particles.columns.x;
```

Canonical grammar insertion: `class_decl = visibility "define" [ "abstract" | "inverted" ] "class" ...`. The stage-one semantic gate rejects `abstract inverted`, inherited/inheriting classes, overriding virtual dispatch, dynamic field layouts, and nontrivial field resources until ownership/destructor lowering exists. Import/export and static methods follow ordinary class visibility rules. No unqualified field widening or implicit AoS-to-SoA conversion.

## Physical storage / ABI

`InvertedArray<Particle>` owns `x[]`, `y[]`, `vx[]`, `vy[]` independently (column contiguous); all columns have identical live row count. No individual mutable `Particle` allocation is required. A row reference is a checked store+slot+generation handle (not a raw pointer); temporary row views are borrowed, nonescaping projections. A materialized single-row snapshot is a value, never a live mutable alias. `to_aos()` / `from_aos()` conversions are explicit and may copy.

The initial native reference admits only pointer-free arithmetic and enum scalar fields; fixed-size POD records remain deferred until the compiler can prove transitive pointer-freedom. Canonical booleans have 0/1 byte representation (no `vector<bool>` bit-proxy ABI). Strings, heap-owning/recursive objects, `Option<T>` tagging, nested flattening, alignment-sensitive FFI, and resource-owning destructors require independent representation decisions and tests.

`push` is all-or-nothing for logical row lengths even on allocation failure; `erase_swap` can reorder physical rows, but surviving handles follow their rows. Deleted handles must fail; reused slots must advance generation; no generation wraparound can resurrect a stale handle. Foreign-store handles fail. `erase_stable` is a distinct optional O(n) operation and must never be silently substituted.

The existing native `DopLayout.hpp` covers homogeneous-width POD geometry and remains authoritative for its current CPU/GPU kernel ABI. The new `DopInvertedColumns.hpp` is a CPU reference collection for **heterogeneous** trivial field types; it is not wired to LLVM IR or device dispatch and does not claim an ABI shared with Java yet.

## Native reference additions / specific limits

The native `InvertedColumns<Fields...>` model currently exposes `reserve(n)`, `push(...)`, `erase_swap(handle)`, `get<I>(handle)`, `set<I>(handle,value)`, `get_row(handle)`, `set_row(handle,...)`, and typed `borrow_read<I>()` / `borrow_write<I>()`. `get_row` and `set_row` take the same store lock to avoid exposing partially updated whole-row values. Resizing/reallocation and row changes are rejected while any field is loaned. Huge allocations can fail without changing logical row count or handle validity.

Owner IDs saturate rather than wrap, and slot generations never revive a stale handle. After owner destruction, a loan cannot issue a *new* span, though an existing raw C++ `span` may outlive its scope if host code misuses it. The source compiler must prohibit such escape; this C++ reference alone is **not** an actor-isolation or teardown-revocation proof.

[Native exact-source carrier audit #39182285383](https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39182285383): 4/4 GCC/Clang normal/ASan+UBSan matrix passed with exact file SHA verification, negative type admission, deterministic row property stress and concurrency cases. This is host-model evidence only, not full LLVM CMake/CTest or Java guest compilation evidence.

## Borrow and concurrency rules

A column view is zero-copy and tied to its owning store; compiler-enforced, nonescaping lifetime is mandatory. Read loans on a field may coexist; a write loan excludes all other same-field access. Disjoint columns may have independent write loans. Structural changes (push, erase, reorder, reserve/reallocate, close, ownership transfer) require quiescence across *all* columns; no suspension or `await` may extend an unsafe exclusive loan without an explicit proof. A raw host-language `span` must not escape its loan; the compiler is responsible for enforcing this when lowered to Oreslang.

A region/task privilege planner should eventually express `reads`, `writes`, `discards` and `reduces(op)`; unsound unknown aliases must serialize or be rejected. `critical` waiting must park the Oreslang task instead of blocking an actor/main/CONTROL scheduler carrier thread.

Each mutable collection has exactly one **owner domain**, which may be an ordinary stack/heap allocation, a structured Task scope, an actor-local arena, or an explicitly authorized shared domain. **Neither `InvertedArray<T>` nor columnar DOP requires an actor.** Disjoint borrowed columns or proven row partitions can be processed by ordinary `nb` Tasks while the owning world/collection remains live. Immutable `Arc<T>`-style shared buffers support zero-payload-copy clones; `rt share` never creates mutable cross-actor pointers. Hot reload and teardown must revoke row handles and loans before reclaiming owning arenas; generations are also scoped to the store identity.

## DOP extensions / ordered roll-out

**P0, CPU correctness:** language keyword and AST metadata; admission diagnostics; typed owning `InvertedArray<T>`; member access and method lowering through row view; field loans; atomic push/erase; handle invalidation; class/struct interoperability tests.

**P1, optimizer-facing:** explicit `Region<T>` layouts (`soa`, `aosoa<N>`, `aos`, `auto`; defaults may not silently violate an inverted-class contract); zero-copy projections, disjoint partition proof, `for ... simd`, field-aware reductions, map/filter fusion, and explicit AoS snapshots. Per-column alignment, heterogeneous widths, AoSoA padding and overflow require checked descriptors before native/Java ABI parity.

**P2, heterogeneous compute:** `compute fnc` effect restrictions, placement, GPU-resident columns, tiling, sparse/gather/scatter, ECS-like joins, AoSoA, device transfer fences, NUMA scheduling and benchmarks; no implicit CPU fallback for unsupported GPU execution.


Actor-independent shared ownership, ECS and structured-task composition are specified in [SHARED_STORAGE_ECS_TASKS_V1.md](SHARED_STORAGE_ECS_TASKS_V1.md). That contract distinguishes zero-copy immutable sharing from exclusive parallel writes and actor-boundary admission.

## Required acceptance gates

1. Parser/formatter and canonical grammar positive cases; reject contradictory modifiers, malformed fields, unsupported inheritance, and use of inverted values without an owning store.
2. Java + LLVM source execution equivalence with AoS for insert, read, update, delete/reinsert, method dispatch, and snapshot conversion; never claim parity from host-only tests.
3. Overflow, allocation injection, bounds, moved/deleted/foreign handles, generation rollover, partial row initialization, logical length equality.
4. Field-sensitive borrow matrix (read/read, read/write, write/write, different columns), row/column view escape, `await`, `nb` task capture, actor crossing, and teardown races.
5. CTest/JUnit under real backend builds with thread sanitizer and ASan/UBSan where supported, property and fuzz tests, SIMD correctness before performance claims.
6. CPU AoS/SoA/AoSoA benchmarks on at least column-only and full-row workloads; GPU claims require actual device execution and transfer-inclusive measurements.

Treat individual checkmarks by stage. Native host-model success never implies guest source support.
