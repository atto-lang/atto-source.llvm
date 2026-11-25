# LLVM native actor Bytes: adversarial STOP/send admission

**Status:** source-only and host-runtime CI gate; **NOT_EXECUTED** on the
source organization while GitHub Actions jobs fail before executing steps.
The source and tests are deliberately confined to the protected-content
LLVM repository. Do not copy them to an unapproved runner organization.

## What this gate adds

`tests/native_actor_bytes_atomic_stop_race_test.c` links the ACTUAL
`NativeActorScheduler.c` and `NativeActorBytesInbox.c`; no mock actor,
mock broker or fake scheduler is used. One actor owns a 4-slot/64-byte
preallocated Bytes inbox. A second actor owns an independent inbox.

1. The first native actor DATA turn authenticates its source-mail ordinal
   and consumes exactly four binary bytes. The test-only callback then
   deliberately holds that actor carrier open. This **host-only test
   orchestration** is not Atto's production suspension mechanism.
2. An oversize Bytes send must return `RESOURCE_EXHAUSTED` without a
   DATA admission, consumed sequence, orphan payload or byte credit.
3. A second DATA frame is admitted and left queued. Four host producers
   and `stop_sealed` are released simultaneously. At most two additional
   senders can win before the STOP barrier; every other sender must fail
   with `MAILBOX_FULL` or `TERMINATED`. No thread uses a guest-level
   blocking send.
4. After the STOP barrier, a new Bytes send and a scalar DATA send must
   return `TERMINATED`. The private inbox's residual bytes must equal
   the number of queued DATA frames; the active turn's bytes are already
   scrubbed.
5. A separate live actor can still receive from its own inbox.
   On STOP/DONE, the stopped actor's terminal receipt must report
   `accepted = processed + abandoned`, with precisely one processed
   message and all queued DATA explicitly abandoned. Actor-private
   queued storage remains allocated until retirement, then the full slab
   must be scrubbed before physical destruction. The peer is unaffected.

This checks send-vs-STOP linearization and cleanup accounting.
It does **not** provide compiler-lowered `.atto` `ActorMail<Bytes>`,
source-level `rt take`, async FSYNC, nonblocking Kafka TCP, complete
actor-private heaps, cross-backend parity or a deployable broker.

## Exact-source execution in an authorized environment

```bash
export EXPECTED_LLVM_HEAD="$(git rev-parse HEAD)"
bash scripts/test-native-bytes-stop-race-exact-head.sh
```

The script refuses a dirty tracked tree, missing exact HEAD or different
Git blobs; it compiles the real native scheduler and inbox at GCC and
Clang O0/O2, repeats each of those four profiles 16 times and runs
Clang ASan/UBSan. Every positive report is explicitly **HOST_ONLY**.
A separate CMake/CTest target is registered for each optimization and
sanitizer configuration on Linux. To count as an exact-head CI proof,
the approved runner must execute nonzero job steps and retain the exact
Git SHA/tree/blob and compiler/exit-status receipts.

## Remaining security and release blockers

- Source branch protection, independent handling of open conformance
  security incidents and source-side approval of a private funded CI
  carrier are prerequisites for any cross-organization mirror.
- `stop_sealed` abandons **scheduler DATA**, while its separately
  preallocated byte slabs are scrubbed only when the lifecycle controller
  performs retirement after DONE. Production binding must close that
  lifecycle gap, fence all producer/consumer loans and zeroize on panic,
  failure, cancellation and process teardown—not depend on a test
  harness manually calling `try_retire`.
- The native scheduler transaction is a C runtime substrate, not
  AttoLang syntax or verified owned transfer. Genuine LLVM and .NET
  compiler-produced actors, nonblocking network and durable Kafka
  execution remain distinct future acceptance gates.
