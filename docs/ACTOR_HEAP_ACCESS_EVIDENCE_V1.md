# Actor heap access evidence v1

This host-only diagnostic records **observed** operations in LLVM's scalar
actor-owned heap. It is not a whole-program data race detector.

The trusted host may call `ores_actor_i64_heap_trace_enable(heap, capacity)`
only before the first actor turn. Capacity must be 1..65536 events. No
hot-path allocation occurs. The recorder stores no guest data values or
native pointers.

Successful scalar slot allocate/read/write/drop and compiler-owned actor-field
initialization/read/write operations are logged under the heap's existing lock.
Each record identifies actor domain, actor ID, incarnation, turn epoch,
slot index, slot generation and operation kind.

`ores_actor_i64_heap_trace_snapshot` refuses partial buffers, missing
recorder activation and active turns. An overflow increments a sticky dropped
counter; dropped events always make trace coverage incomplete. The recorder
is cleared at physical heap reclamation.

## Explicit limitations

- Memory outside ActorOwnedI64Heap is not traced.
- Actor turns are serialized and epochs alone are not independent threads.
- No scheduler, mailbox, Future, mutex or FFI synchronization events are
  currently collected by this recorder.
- No native runtime provenance or complete access coverage receipt exists.
- A clean observed-access log never certifies program race freedom.
- This changes no Oreslang compiler lowering outside existing scalar helpers.

The new `native_actor_i64_access_evidence_host_only` CTest checks event
identities, operation kinds, two turns, activation refusal, and overflow.
Exact-head authorized CI plus sanitizer runs remain required for promotion.
