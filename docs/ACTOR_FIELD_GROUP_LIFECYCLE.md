# Compiler-generated i64 actor fields: ActorGroup lifecycle bridge

This integration extends the restricted LLVM `define actor` source lowering
in PR #46. It reuses `ActorGroupLifecycle.hpp` rather than creating another
supervisor registry, heap allocator or actor-reference type.

## Native guest data / Java–Graal parity contract

| Invariant | LLVM implementation | Java/Graal analogue |
| --- | --- | --- |
| Actor lifetime | One persistent `OresActorI64Heap` per actor ID + incarnation | One logical actor-owned memory slice |
| Field layout | Source declaration order, zero-based fixed `i64` indexes | Typed per-actor `i64` region indexes |
| Ownership | Opaque actor heap + nonescaping field record | Actor-owned region with generation-checked slots |
| Admission | One logical turn, possibly new physical carrier | Serial actor mailbox, not thread affinity |
| Ready barrier | After generated initializer creates the record | `await worker.get_ready()` |
| Stop/cancel | Stop request revokes new adapter turns | Supervisor CONTROL requests stop |
| Done barrier | Only after generated field drop, native quiescence and physical release | `await worker.get_done()` after finalization |
| Suspension / FFI | Heap loans pin memory, without pinning carrier threads | FFM loan fencing until completion |

The generated methods `ores_actor_NAME_init`,
`ores_actor_NAME_METHOD`, and `ores_actor_NAME_drop` are internal host-only
entrypoints. LLVM passes the scheduler's opaque heap and current domain/epoch,
not guest-forgeable Oreslang values. ActorGroup keeps a reference/lifecycle
ticket. `GeneratedActorFieldWorker` owns the heap and field record and uses
ActorGroup's existing `mark_ready` and `finalized` APIs.

## Shutdown sequence

1. Supervisor invokes stop, cancellation or a terminal panic signal.
2. Adapter forbids any new guest method turns. Already admitted turns finish.
3. Trusted scheduler fences stale resumes, timers and external FFI callbacks.
4. `poll_finalized()` returns *pending* while a turn or native loan survives.
   It does not park a carrier or manufacture a terminal `done` event.
5. When quiescent, enter one internal finalizer turn and invoke the
   compiler-generated field-drop entrypoint exactly once.
6. Retire the actor heap and physically destroy it.
7. Acknowledge `ActorGroup.finalized()`; only now is `get_done()` settled.

There is no Java tracing collector in the LLVM field-data path. Its C native
allocations can still have allocator or page-fault costs. Java/Graal keeps
host control objects on a managed heap; native `Arena.ofShared()` reduces
*guest data* GC pressure, not all JVM pauses.

## Tests and limitations

`native_generated_actor_group_lifecycle` links **actual generated LLVM IR**
for the Oreslang `Counter` actor to the native C heap and C++ ActorGroup.
It tests persistence over carrier migration, ready/done ordering, cancellation,
a simulated handler panic, loan-pinned physical memory, stop with an active
turn, and invalid entrypoint admission.

This is an **explicit trusted host integration slice**, not end-to-end source
`spawn` lowering or a fully production-owned ActorGroup scheduler. Guest
`await` continuation lowering, asynchronous supervisor CONTROL dispatch,
general classes/structs, nested actor graphs, actor data migration across
isolates, and AOT/FFI device tests remain separate prerequisites.
Caller-controlled host references must be fenced before finalization;
unmanaged raw pointers remain unsafe. This C ABI only supports bounded
scalar `i64` fields, not a stable general-purpose binary schema.
