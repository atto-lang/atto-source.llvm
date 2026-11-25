# Compiler-lowered packed source-actor state

The native LLVM source-actor path now stores the actor's bounded scalar field
set as **one persistent ActorArena record per actor incarnation**, instead of
allocating one independent scalar heap slot per field.

This is the first compiler-lowered aggregate actor-state milestone. It is
deliberately **not** the compiled-general heap claim: ordinary class, record,
array, list, map and closure allocations are not yet routed into this record
or into `ActorOwnedGraphHeap`.

## Storage and authority

- `ActorPackedState` is a compiler/runtime adapter over the existing
  `ActorArena`; it does not introduce a second allocator.
- The adapter allocates exactly one persistent record sized
  `field_count * sizeof(int64_t)`. Field index selects a byte offset inside
  that record.
- Generated LLVM resolves only opaque packed-state field accessors. The arena
  pointer, record handle, actor identity and turn credentials remain trusted
  scheduler/runtime capabilities and are never source-language values.
- Turn authority inherits `ActorArena` carrier binding. A copied domain/epoch
  is insufficient on another native thread; carrier migration requires
  `leave` followed by a fresh `enter`.
- Constructor field initialization is single-use. The adapter records which
  fields are mutable, so a generated write to an immutable field still fails
  closed even if compiler validation regresses.
- Actor retirement revokes fresh arena access. Physical release waits for the
  admitted turn to leave, then zeroizes/frees the persistent arena record and
  adapter metadata.

## Scope

Existing source syntax remains bounded scalar actor fields. This change proves
that the compiler/runtime can retain one aggregate state image across mailbox
turns and physical carrier migration without tracing GC. It does **not** yet
support source actor fields whose types are classes/records/containers, field
borrows, suspension-safe aggregate loans, or guest-visible arena handles.

The next promotion step is compiler allocation of ordinary mutable aggregate
objects into the actor-owned graph/arena domain, with generated drop/borrow/FFI
lease glue. Only that broader path can justify **compiled-general**.
