# Native immutable shared-payload scheduler integration (host-only draft)

**Status:** SOURCE AUTHORED / NOT_EXECUTED at this integration head. Neither a
compiled `.atto` actor transport nor a cross-backend zero-copy certification.

## Dependency and transport model

- Stacked on LLVM #1746 (stable callback identity), #1741 (bounded scheduler
  DATA+Bytes atomic admission), and imports the immutable descriptor substrate
  from LLVM #1742. The source of truth for the imported header and baseline
  host tests is #1742; do not merge duplicate header implementations into
  main without reconciliation.
- `SharedImmutablePayload::publish` moves a producer-owned byte vector to
  refcounted immutable shared backing. Host code must not retain mutable
  aliases. This is a **trusted host precondition**, not yet a checked Atto
  `rt share` implementation. A slice charges the full backing allocation.
- `SharedPayloadInbox` preallocates bounded queue metadata and source-mail
  sequence slots. Binding is permanent and permitted only on a pristine inbox.
  Once sequence-bound, legacy `try_send` and `try_receive_result` reject,
  and only strictly increasing scheduler DATA ordinals can be admitted.
- `NativeSharedPayloadSchedulerBinding` binds the scheduler's exact actor
  ref/generation and the descriptor inbox while the scheduler lock excludes
  STOP, legacy DATA and competing sends. The callback identity is a **single
  linked C-compatible symbol**, not an inline header-local address.
- `native_shared_payload_try_send_atomic` submits the immutable descriptor
  through the scheduler's existing `try_send_committed` transaction. The
  scheduler holds its mutex while the shared inbox `try_lock` admits the
  descriptor without copying backing bytes or allocating ring nodes. If the
  callback declines, **no** scheduler DATA ordinal/credit is admitted. If
  accepted, no fallible step exists between descriptor and DATA publication.
- `native_shared_payload_receive_current_data` first validates an active
  scheduler-issued physical DATA turn for that exact actor/generation, obtains
  its source-mail sequence and then dequeues only the matching descriptor.
  External callers, stale references, foreign turns and wrong sequences
  cannot dequeue the committed slot.

## Verification gates

Host reference test:
`native_shared_immutable_payload_host`

Native physical scheduler:
`native_shared_payload_atomic_scheduler_host_only`

Sanitizers:
`native_shared_payload_atomic_scheduler_asan_ubsan`

The source-authored tests check legitimate zero-copy backing identity across
sender/receiver, ordinal mismatch refusal, legacy mode denial, distinct
callback refusal, failed capacity admission with zero queued descriptors,
stale actor-generation refusal, terminal accepted/processed counts and
STOP/DONE before queue closure.

**No executed CI receipt has been produced yet.** A workflow with
`steps=[]` / `runner_id=0` is a runner-admission failure, not a compiler
or runtime verdict. Run exactly this PR head with nonzero steps, strict
C++20 compilation, Clang/GCC O0/O2 and ASan/UBSan. Record Git HEAD/tree,
compiler versions, source hashes, exact test commands and test count.
The existing source-org runner has repeatedly failed admission; there is
no claim that its workflow conclusions constitute test results.

## Security and lifecycle limits

1. The scheduler calls trusted native C/C++ callbacks. The callback wraps
   exceptions to prevent unwinding through a C lock. The API is **not exposed
   to guests** as FFI or raw pointer access.
2. Although the send path does not allocate payload bytes or ring nodes,
   mutex contention, atomic refcount operations and last-reference
   destruction are not a certified real-time/zero-GC guarantee. An actor
   carrier must not spin or block on a full/busy channel; source sends will
   need a scheduler-owned `suspends` wait/credit protocol.
3. Actor STOP/DONE does not automatically destroy a separately owned
   descriptor inbox. The runtime must seal admission, quiesce all physical
   operations, refund quotas, close the inbox and reclaim outstanding
   descriptors only after loans/I/O have completed.
4. Authentication remains through trusted native scheduler authority. Source
   compiler lowering and canonical borrow provenance must prevent copying
   mutable aliases, storing borrowed spans past turn/lifetime, stale tenant
   authority and raw guest pointer escapes.
5. Cross-tenant sharing remains prohibited without explicit admission.
   Per-actor queue quotas here do not establish aggregate per-tenant quotas,
   process-wide memory budgets, crash-safe persistence or remote transport.
6. The current scheduler's `bind_bytes_only` function name is historical;
   its callback-pinned mode now also handles host-only immutable descriptors.
   It must be renamed or documented as a generic payload lane before
   exposing a stable compiler-facing abstraction.

**Next gate:** execute and fix host tests, then compile a real `.atto`
supervisor+actor channel fixture on LLVM, Java and .NET and run positive and
negative ownership cases. Do not merge as certified source-level zero-copy
until that evidence and independent review exist.
