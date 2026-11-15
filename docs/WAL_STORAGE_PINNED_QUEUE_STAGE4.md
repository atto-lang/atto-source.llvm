# Oreslang DB — LLVM Stage 4: actor-incarnation WAL queue lifetime pin

**Draft, native host-side integration only.** Stacks on `oreslang-source.llvm` PR #1238; tracks LLVM #1231 and database #4. No typed `.ores` builtin, no real isolate-issued tenant grant, and no native guest execution.

## Concrete source

`WalStoragePinnedDiskQueue` owns a `WalStorageDiskQueue` and retains one `NativeActorRef` generation-fenced resource loan **before starting the disk and completion workers**. It checks `actor_id` and `generation` against the trusted runtime-issued WAL scope. All host submissions require the same opaque `NativeActorRef` incarnation ticket, checked through `same_incarnation` (not integer equality). It does **not** derive tenant/isolate/shard authority from the actor ref, nor authenticate the host-supplied activation pointer or directory custody.

An off-carrier destructor destroys and joins the underlying queue's disk and completion workers, which own the file descriptor, **before** releasing the actor resource loan. Thus native group finalization cannot reclaim an actor arena while the queue owns a live physical storage operation. A failed queue constructor releases its retained resource loan after unwinding; a finalized actor refuses a fresh loan.

The production-header test in `tests/native/wal_storage_pinned_disk_queue_test.cpp` uses the real `NativeActorGroup` and `RuntimeFinalizer` APIs. It checks actor loan retention, foreign runtime ticket rejection, physical WAL sync, STOP/group finalization blocked while the queue exists, off-carrier teardown, and successful finalization after workers retire.

## Verified / not verified

Local **host-only** GCC C++17 strict compilation and 11/11 checks passed five times; Clang and GCC ASan+UBSan also passed 11/11. **Those local runs used an explicit test-only `ActorGroupLifecycle` shim** because this working container does not have the whole private native repository. The test-only shim is **not committed**. `scripts/verify-wal-storage-pinned-disk-queue-stage4.sh` deliberately uses the actual production `ActorGroupLifecycle.hpp` in the repository, so no CI/native production-header pass is claimed until that script executes with nonzero steps and pinned toolchain/tree.

## P0 to complete

1. The trusted scheduler must create/own the queue and *explicitly revoke it on actor STOP, rollback, hot reload, and tenant retirement*. This loan **prevents premature arena retirement but does NOT itself forbid new submissions after STOP**. An opaque actor ticket is not a live-turn execution claim.
2. Integrate **per-operation** `AsyncIoOperationLifecycle`/`AsyncIoActorLifetimePin` and scheduler CONTROL continuation handoff, including exact generation/operation IDs, cancellation vs physical retirement, producer/grant release and reconciliation. One conservative queue-wide pin is not a substitute for these operation receipts.
3. Verify `openat2`/directory-relative exclusive FD custody, no links/traversal/mount surprises, synced creation directory, cross-tenant global worker budget and real isolate-issued permissions. Numeric scope and raw activation pointers remain host-trusted inputs.
4. Execute exact-head real native compiler build and test with production headers, instrumented fault injection, crash/restart WAL replay, durable idempotency, guest `.ores` tests and Java parity. No claim of power-loss durability follows from `fdatasync` host tests.

Keep draft and require independent approval/protected branches before merge. No private cross-organization mirror was performed. Zero-step Actions are runner-admission failures, not test evidence.

## Stage 5 — native STOP admission fence

`NativeActorRef.resource_admission_open()` checks the scheduler ticket's
`stop_issued`, `detached_from_group`, `physically_done` and terminal resource
bit without allocating. The pinned WAL queue rejects a stopped incarnation and
revokes its worker. Tests assert open before STOP, closed after group.close(),
and a negative post-close submit while preserving actor resource-loan retirement.

Local GCC/Clang strict C++17 and GCC ASan/UBSan test-only shim: 14/14.
**NOT EXECUTED** against the actual production ActorGroupLifecycle header;
that separate exact-head gate must run before claiming native integration.
This precheck alone cannot arbitrate STOP against a concurrent submit:
the supervisor MUST call `revoke()` on STOP and retain kernel-operation
lifetime pins and generation-fenced completion reconciliation.
