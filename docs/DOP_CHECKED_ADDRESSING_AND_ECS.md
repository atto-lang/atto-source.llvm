# Checked data-oriented addressing (native reference slice)

Status: **host-only CPU substrate**; not an implemented source-language
`ECSWorld`, actor-owned state-loan integration, general ECS entity manager,
LLVM source-expression lowering, GPU access layer or verified permission gate.

## Architecture

Oreslang entity identity, actor references, and physical addresses remain
**different abstractions**:

- **ECS** represents logical entities and their components/archetypes. It is
  not a fourth alternative to AoS, SoA, and AoSoA.
- **AoS / SoA / AoSoA** are physical placement strategies.
- **DenseIndex32<Tag>** and generation/epoch-protected handles are preferred
  over native pointers for relocated ECS components.
- **ActorRef** remains a message capability, never a compact heap offset.
- **Actor-local compressed32 managed references** are separate from ECS row
  handles and are not native FFI pointers.

`runtime/DopCheckedRegion.hpp` provides a small checked fixed-shape
homogeneous numeric/enum field store backed by the existing
`oreslang::dop::Plan` geometry. Its only address-bearing state is held
internally; the API returns no raw pointer or mutable span.

### Supported operations

- Construct with an actual validated `Plan` (AoS, SoA, or AoSoA).
- Mint a `CheckedRegion::RowHandle` from a valid row.
- Read/write numeric or enum scalars of **exactly** the plan's ABI width.
- Reject out-of-bounds rows and fields, wrong scalar sizes, and row handles
  from another store or an earlier layout epoch.
- Explicitly `relayout` with unchanged logical shape. Copy each logical cell
  using checked offsets, leaving freshly allocated padding zeroed.
- Commit a layout migration only after allocation/copy succeeds; increment
  epoch on success. Prior row handles become stale.

`RowHandle` has no public constructor, but is **not** a cryptographic proof
or an authorization capability. The owner counter is process-image-local;
this is a native checked host data type, not a protection boundary against
malicious native code or fabricated compiler IR.

```cpp
using namespace oreslang::dop;
CheckedRegion positions(Plan(Layout::AoSoA, 1024, 2, 4, 16, 8));
auto row = positions.row(7);
positions.write<std::uint32_t>(row, 0, 123);
positions.relayout(Plan(Layout::SoA, 1024, 2, 4, 16, 1));
// positions.read<std::uint32_t>(row, 0) rejects: stale epoch.
row = positions.row(7); // Resolve fresh handle.
```

The backing `std::vector<std::byte>` is host memory; typed reads/writes use
`memcpy`, avoiding alignment UB for byte-buffer-based storage. Unsupported
scalar representations (pointers, bool, strings, arbitrary objects) are
compile-time rejected. This phase does not encode Oreslang's canonical
boolean or record ABI.

### Actor ownership

The caller must already have exclusive actor execution authority. The store
is intentionally not thread-safe. Do not share it across concurrently
executing domains, escape it across an actor message, or retain an implicit
physical address across `await`, structural mutation or re-layout.

A future source-level `state { world: ECSWorld }` actor adapter must use
verified `critical handle_state` loans, establish actor execution/domain
identity on continuation resumption and prohibit layout migration while
active component views/loans remain. **This header does not implement or
enforce those actor admission/borrow barriers.**

For a general ECS, stable entity IDs must have generation checks and map to
the current dense physical row. The current fixed-shape `RowHandle` is
intentionally **epoch-invalidated by relayout** and does not provide stable
entity identity or sparse-set slot reuse. Do not use it as an `EntityId`.

## Static analysis and performance requirements

The compiler's logical place must include owner domain, region/store
identity, component/field, row interval, storage epoch, access mode, and
whether a raw address has escaped. No safety certification can be based on
user-supplied strings or untrusted summaries.

Future source integration must provide:

1. Authenticated typed/HIR access binding and alias/borrow-checker proofs.
   An explicit structural change or layout epoch increment invalidates
   outstanding row/component addresses; no raw address crosses suspension.
2. Checked native lowerings for AoS/SoA/AoSoA including pointer-width and
   signed-overflow handling. FFI ABI pointers remain native-width and require
   an explicit scoped pin/copy contract.
3. Interprocedural diagnostics for stale address escape, unverified ECS
   generation, actor-domain mutation, false-sharing risk and budgeted
   actor/ECS amplification. Metadata-only `DataLayoutAdvisor` suggestions
   are **advisory**, not a cache benchmark or compiler optimization proof.
4. Target/architecture measurements (AoS versus SoA versus AoSoA) for
   cache misses, SIMD throughput, memory footprint, structural mutation,
   allocation churn and end-to-end actor p99. Never assert AoSoA always wins.
5. Full exact-tree LLVM/CMake/CTest, sanitizers, verified guest compiler
   regression programs and Java/Graal differential conformance before
   promoting any source-language compatibility claim.

## Focused verification

CTest case: `native_dop_checked_region_addressing`

`tests/dop_checked_region_test.cpp` runs AoS/SoA/AoSoA row/field matrices,
several tile widths, round-trip read/write, foreign and stale handle
rejection, failed-migration strong exception guarantee, all six directed
layout transitions, signed-zero/NaN bit preservation and checked geometry
overflow. All assertions are active even under `NDEBUG`.

An isolated exact-file local compile/test of this header plus the actual
`DopLayout.hpp` Git blob is a **component verification**, not proof that
the entire repository compiler builds or that Oreslang guest syntax runs.
