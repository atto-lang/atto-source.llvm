# Actor-local inverted class MIR: phase-zero admission

**Draft compiler gate only.** This adds a C++17 typed MIR admission contract
for future `.atto` inverted classes. It is **NOT wired into** the production
AttoLang parser, compiler CLI, LLVM codegen, actor-owned arena allocator,
scheduler authentication, or any other backend. A passing host test is NOT
executed guest Native AOT evidence and not release/cross-backend certification.

## Accepted shape (future source spelling; not yet parsed by this change)

```atto
use namespace workers;

define inverted class Particle as
    let x: int;
    let y: int;
end

mtd (mut self: Particle) move_by(dx: int, dy: int): void {
    self.x += dx;
    self.y += dy;
}
```

A future compiler producer may construct `class_mir::Schema` with two
`ScalarKind::I64` fields, a bounded 1..256 capacity and the declaring actor
scope, then lower the abstract local operations:
`Allocate -> Push -> CallMoveBy -> ReadField -> Snapshot -> EraseSwap ->
ReadSnapshotField -> Close`. A source handle is a **local symbolic binding**,
not an address or guest-mintable token. Snapshots are detached values; they
remain readable after the original handle is erased.

## Fail-closed typed restrictions

- Exactly one named namespace/actor/class and **two distinct i64 fields**.
  `bool`, `f64`, `Pointer`, capacity 0 or capacity >256 reject.
- An operation's compiler-declared actor scope must match the owner; a future
  backend must **separately authenticate the actual executing actor** using
  trusted scheduler/runtime state. The public `acting_actor` integer is *not*
  such a credential, and must never be read from an untrusted guest.
- `CallMoveBy` requires the `move_by` snake_case receiver method, the
  matching nominal class, and mutable `self`. No arbitrary dynamic dispatch.
- Live handle use is checked; erased bindings are stale forever, even when a
  physical row slot is reused. Symbolic handles cannot be shadowed by a
  detached snapshot binding (or vice versa). No guest pointer is constructed.
- `Suspend` and `ExportHandle` reject even if no outstanding loan is visible:
  no proven pinned-frame or cross-actor ownership contract exists yet.
- Allocation must be first; owner closure must be terminal. All metadata is
  bounded to 4096 source instructions and finite, ASCII identifiers.
- For `CallMoveBy`, MIR requires owner, actor, generation, bounds and
  **checked-arithmetic** flags. A real emitter must implement these checks and
  compute both i64 additions before committing either field.
- For pushes, MIR requires owner/actor/capacity. For snapshots, guards check
  the source row at capture time, but detached field reads do not resolve
  the row after erase. `import_checked` independently validates the guard
  flags and source lifetime contract on imported MIR.

The flags are verification obligations, **not actual machine-code guards**.
The host C++ compiler uses dynamic containers for its own analysis; that does
not imply guest GC and cannot establish a production zero-GC runtime.

## Executed evidence

`tests/inverted_class_mir_admission_test.cpp` was compiled and **actually
executed locally** against the exact two GitHub source Git blobs:

- GCC 14.2 C++17 `-O0` and `-O2`
- Clang 17 C++17 `-O0` and `-O2`
- GCC `-O1 -fsanitize=address`
- GCC `-O1 -fsanitize=undefined`
- GCC `-O1 -fsanitize=thread`

All seven local executable runs passed. The regression rejects absent or
forged guard requirements, wrong actors, incorrect receiver mutability/types,
unsafe methods, stale handles, bad snapshot bindings, export/suspend,
missing close, name shadowing, invalid schemas and oversized programs.
Source-owned GitHub Actions and CTest are separate evidence gates; do not
count no-step Actions failures as compiler failures or test passes.

## Required next compiler work

1. AST/parser syntax and namespace-bound receiver `mtd` type checking,
   including multiple actor types in the same file and sealed state access.
2. Bind actor identity to an actual trusted runtime capability/actor arena.
   Do not accept guest-supplied numeric IDs as authority.
3. Implement generation-checked 32-bit actor-relative handles, dynamic bounds
   and liveness traps, and two-field checked arithmetic with atomic rollback
   in native LLVM IR. Forbid pointer aliasing and borrow lifetime escapes.
4. Execute real `.atto` source through AST -> independently verified MIR ->
   LLVM IR -> Clang native `-O0/-O2`, plus ASan/UBSan/TSan as appropriate.
5. Independently align Java/GraalVM, Wasm, Forge and .NET with shared executed
   conformance contracts before claiming five-backend parity.

The private source-transfer security incidents #22/#77/#127/#212/#222 are open,
source `main` is unprotected, and the private carrier manifest does not
authorize this branch. No private compiler source may be mirrored to another
organization to bypass those gates. Keep the feature draft/unmerged until
independent review and genuine executable compiler evidence.
