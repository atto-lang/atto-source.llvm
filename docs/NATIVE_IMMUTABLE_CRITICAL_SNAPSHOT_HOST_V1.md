# Native immutable snapshot root + critical publication — bounded host primitive

**Status: HOST-ONLY. Compiled AttoLang guest `critical` and immutable
Map/Set/List lowering remain NOT_EXECUTED.**

This implementation is deliberately separate from existing LLVM actor-local
`ActorCompactFieldStore`, `ActorOwnedGraphHeap`, and `CriticalGate` work,
and from conformance draft `immutable-containers-critical-v1` (#610).

## Behavior

`oreslang/runtime/CriticalImmutableSnapshotRoot.hpp` implements a fixed-size
shared-region snapshot root with 64-bit monotonic root revision and
generation-checked slot leases. Its immutable scalar payloads are *not*
actor-private heap Ref32 addresses. It is a bounded host-level publication
primitive: a logical user-facing `PersistentMap/Set/List` must be authored
in portable `.atto`, not replaced by this fixed scalar-array model.

- A snapshot pins one published generation and obtains root and revision in
  the same short control operation. Existing snapshots remain immutable after
  root replacement.
- A writer stages a new root by reserving a free slot and copying immutable
  bytes **outside** the publication gate. A release store completes staging.
  The `try_publish` operation uses an acquire state check and a
  non-blocking `try_lock` to atomically compare revision and publish
  root+version. No wait, callback, FFI, allocation or suspension in publish.
- A stale revision loses without changing the current root. Its candidate
  must be discarded explicitly. When the last reader releases an old version,
  that version is scrubbed and its bounded slot returned to the pool.
- `Busy`, `Conflict`, `Capacity`, `Stale`, and `Exhausted` remain
  distinguished; callers can schedule retries **outside** a critical body.
- Lease/Candidate objects are noncopyable and nonmovable, with explicit
  release/discard. Leaks consume bounded resources rather than aborting the
  host process, but integration MUST add supervisor-scoped cleanup and
  generation fencing before user code can access this primitive.
- This host-level shared immutable root has its own lifetime/ownership
  boundary. A 4-byte actor-relative reference field cannot be copied into
  this structure and become a shared authority.

## Actual native host verification

The `native_immutable_critical_snapshot_test.cpp` fixture tests:
old-root preservation, first-writer-wins conflict, pinned reader retirement,
double-release rejection, bounded capacity denial and recovery, plus
**six simultaneously running C++ threads** each publishing 700 updates.
Expected final revision and value: **4,200**, with no lost updates.

The source header and test were independently compiled from **byte-exact
Git blobs**, not merely equivalent-looking local text:

- Header: `d04730f89299713e31a4c2329083d79ce2cd6882`
- Test: `abd5fc4c8927e568460a41b78ae26e119ce3a793`

Locally executed at these exact source blobs (2026-11-08):

- Clang 17, C++17, `-O2 -Wall -Wextra -Wpedantic -Werror -pthread`: PASS,
  6 workers / 4,200 published updates per run, multiple runs.
- Clang 17 with AddressSanitizer + UndefinedBehaviorSanitizer:
  PASS, 6 workers / 4,200 published updates.
- GCC C++17 `-O2 -Wall -Wextra -Wpedantic -Werror -pthread`:
  PASS, 6 workers / 4,200 published updates.

This is **not** ThreadSanitizer evidence, compiler-generated guest execution,
a `.atto` frontend/lowering test, or a NativeAOT GraalVM/.NET result.
GitHub Actions may fail with `steps=null`; such failures do not count
as test execution or parity. Remain DRAFT pending exact-head CI, independent
review, and verified integration.

## Next compiler/runtime steps

1. Lower `critical { }` via the existing logical `CriticalGate` scheduler
   contract, never an OS mutex that blocks the carrier across user code.
2. Lower `PersistentMap/Set/List` and shared-root revision checks from
   typed `.atto`, enforce transitively immutable Sendable and scoped
   shared-region permissions, and reject any actor-local ref32 escape.
3. Test both declaration modifier orders with `critical` and `suspends`;
   reject awaits, I/O and blocking operations in critical bodies.
4. Add owner-incarnation fencing and supervision cleanup for abandoned leases
   and pending candidates, then validate with real two-carrier guest fixtures.
