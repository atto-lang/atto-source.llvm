# Native tagged buffer DATA — host ABI v1 (draft)

This work extends the **real C11 NativeActorScheduler**, which previously
delivered only a `uint64_t` scalar DATA callback. This is **not** Oreslang source
`.ores` buffer messaging, zero-copy guest execution or a sandbox guarantee.

## Typed message and admission

The original `ores_native_actor_scheduler_try_send(..., uint64_t)`,
`OresNativeActorTaskFn`, CONTROL, wake-priority and legacy scalar scheduling
remain intact. The preallocated bounded per-actor FIFO now stores an
explicit tagged union: `kind=0` for scalar DATA and `kind=1` for a
four-u64 `OresNativeOwnedBufferTicket` (tenant epoch, type schema, logical
buffer identity, owner generation). The typed item **never** uses the scalar
callback, passes a heap address or smuggles its ticket through an i64.

The host calls `ores_native_actor_scheduler_configure_owned_buffer()`
before the first carrier or message admission to install four trusted hooks:

- `on_validate` authenticates the exact source ticket, ownership, schema,
  incarnation and exclusive transferability. It is called before exposing
  unknown-recipient, terminal or capacity statuses, without modifying ownership.
- `on_commit` performs an atomic ownership-generation change once the
  generation-matched recipient is known live, the mailbox has capacity and
  workers are serviceable. A failed commit **must** preserve the sender. A
  successful commit must preserve tenant/type/id and **strictly increase**
  generation; the scheduler aborts on a broken successful callback.
- `on_delivery` receives the ticket and **exact recipient ActorRef generation**
  on that recipient's authenticated physical actor turn.
- `on_discard` receives that exact recipient ActorRef and reclaims queued
  recipient-owned DATA abandoned by `stop_sealed`.

All hooks are trusted host functions; they must be exception-free,
allocation-free, nonblocking and must not reenter the scheduler. The validator
and commit run **under** the scheduler admission mutex; delivery runs on
the actor's exclusive worker; STOP discard runs **outside** the scheduler mutex.
This mutex/callback lock ordering is not yet integrated with the C++ pool
or SourceActorBootstrap's group mutex. A runtime integration must prove that
the pool/gate never takes scheduler locks in reverse order.

`try_send_owned_buffer` accepts only an exact, current scheduler-owned
`sender` with a live **thread-local physical execution claim**. It refuses
ambient main-thread calls, wrong-source identities, self-transfers and
unconfigured callbacks. It returns established invalid/terminated/full/
resource-exhausted results, plus the new
`ORES_NATIVE_SEND_OWNERSHIP_REJECTED` result for invalid or uncommitted
buffer authority. Successful admission acknowledges *queue acceptance*,
not completion of receiver code or durable persistence.

## STOP and lifecycle

`stop_sealed` seals the receiver under the same mutex used by admission,
atomically marks queued messages abandoned for terminal accounting and
detaches their FIFO cursor. Each accepted typed envelope is then dispatched
to the trusted discard hook **once** outside the mutex. Pending cleanup
counts block terminal publication, actor `wait_done`, scheduler `drain`
and physical `try_destroy` until all callbacks finish. Scalar queued
messages need no destruction. Already-active DATA callbacks are allowed
to return; a typed handler owns responsibility for consuming or dropping
the delivered ticket. An unsafe/disrupted discard callback is a fatal
host contract violation and never a successful DONE result.

Memory remains preallocated at scheduler bootstrap. The new envelope
increases per-mailbox slot size; it is not yet qualified for bounded
worst-case execution time, embedded profiles or hard real-time operation.

## Tests and evidence boundaries

`tests/native_actor_owned_buffer_scheduler_test.c` is registered as
`native_actor_tagged_owned_buffer_stop_cleanup` (strict C11). It uses
**mock ownership tickets**, not real pool allocations, and checks:

1. external send denied without a physical source-actor turn;
2. mixed scalar/typed FIFO, exact receiver generation and typed callback;
3. full mailbox preserving ownership (commit hook must not execute);
4. forged ticket rejected before mailbox-full status;
5. sealed STOP dropping queued accepted typed DATA exactly once outside
   the scheduler callback, with consistent terminal accounting;
6. stopped receiver sends denied without transferring ownership;
7. successful typed delivery to a different live receiver; and
8. ownership-commit refusal without an accepted message.

`.github/workflows/trusted-actor-buffer-exact-head.yml` now includes GCC
and Clang C11 ASan/UBSan jobs for this fixture, all pinned to the actual
PR head and required blob hashes. `tests/verify_trusted_actor_buffers.sh`
adds these two sanitizer executions and GCC TSan to its four existing
host C++ tests, requiring 15 total successful executions before printing
its completion receipt. `EXPECTED_SOURCE_TREE` additionally permits an
**already-authorized** mirror commit to prove exact Git tree equality even
if its commit SHA differs.

**Execution state:** these new native C changes and test fixtures have been
committed, but no executable exact-head native or compiler result has yet
been observed. The source organization Actions fail with zero job steps.
The source private-carrier allowlist currently authorizes no destinations,
so no private source has been mirrored to another organization. Earlier
host C++ sanitizer receipts apply only to older blobs.

## Remaining P0

A C++ adapter must connect this callback contract to the existing
`TrustedActorBufferPool` and authenticated scheduler gate, without
allocation during commit, reentrant locking or a guest-forgeable ticket.
The native `SourceActorBootstrap.c` path currently only calls the scalar
`oreslang_source_actor_send(void*, i64)`; it must gain a separate typed
ABI and correct group-lock/STOP callback lifecycle. Only after that should
the Oreslang AST/HIR/MIR checker admit typed `on_message(owned Array<int>)`,
`rt take`, `Sendable` and a positive source-to-LLVM-to-native test. Keep
all relevant PRs **DRAFT** until real native execution receipts exist.
