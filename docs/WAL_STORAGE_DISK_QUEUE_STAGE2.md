# Native WAL disk queue — isolated stage 2 (LLVM backend)

**Draft**, stacked on LLVM PR #1234. This is a native host-side disk queue that invokes the existing blocking POSIX WAL owner off the submitter thread. It is **not** an integrated Oreslang async-I/O runtime, isolate-issued unforgeable capability, durable DB commit API, or compiler-executed guest. Tracks #1231 and DB PR #11.

## Implemented

- `WalStorageDiskQueue` **owns** a preverified `WalStorageFdOwner`, the underlying file descriptor and exactly one physical disk worker + one completion worker. `submit` never calls `pwrite` or `fdatasync` on the caller thread. Its current API returns a host-only `std::future<SyncedReceipt>`, not an Oreslang `Future<T>`.
- Total admitted but unretired operations are capped at 32, **including** executing disk work and queued completion; each frame is capped at 1,118 bytes **before defensive copying** (maximum 35,776 outstanding frame bytes). The underlying owner also validates complete ODB1 frame, signed IDs, checksum, monotone sequence, duplicate op IDs and 1 MiB WAL budget.
- Actor identity is checked against the **host-supplied pointer identity** and scope, but the native isolate must still independently authenticate that pointer; never use guest data/FFI as issuer. Queued requests after revocation or physical failure become `UnknownOutcome`; no retry/skip/rollback, no `Committed` acknowledgment.
- Completion uses a **separate** worker and `std::promise`, which does not execute user callbacks inline. Before destruction, queue revokes and joins physical worker and completion worker, then its FD owner closes. This destructor **may BLOCK** and can therefore run **only on a trusted off-carrier retirement worker**, never an Oreslang actor carrier or a physical I/O worker. This package does not yet implement actor-retirement/NativeActorRef pins.
- Production builds do not include the optional deterministic `ORES_WAL_HOST_FAULT_TEST` worker hook. Tests use it only to hold a disk request for the 32nd/33rd admission and healthy-tenant-progress oracles.

## Evidence

Local GCC 12+/Clang C++17 `-Wall -Wextra -Werror -pthread` compile and 22/22 host assertions passed, including real `pwrite`/`fdatasync`/reopen, detached source copying, foreign generation, 1119-byte frame rejection before copy, exactly-32 admission, 33rd rejection, healthy tenant progress while first worker stalled, 32 futures resolved as `UnknownOutcome` on revocation, read-only FD failure quarantine. Additional sanitizers are a separate evidence class. `scripts/verify-wal-storage-disk-queue.sh` inherits exact commit/tree/compiler hashes from previous native test script. These are *host-native tests*, not `.ores` guest compiler proof, process crash/ACK safety, or true power-loss durability.

## P0 missing integration and safety hazards

1. **Runtime-created isolate/tenant/actor generation grant** and physical FD custody with Linux `openat2` or equivalent race-free scoped open, exclusive regular inode lease, symlink/hardlink protections and parent-directory fsync. Existing caller-supplied `Scope` / activation pointer is only a host seam.
2. `AsyncIoOperationLifecycle` + `AsyncIoActorLifetimePin` integration; preserve retirement loans until physical and completion quiescence, prohibit destruction from actor carriers; ensure actor STOP/rollback/quarantine does not free a native arena or descriptor prematurely. No scheduler callback runs in this queue; isolate must enqueue its own authenticated control continuation.
3. Bounded executor-wide and tenant-wide scheduler admission across *all* shard queues. Local 32-slot limit alone does not prevent unbounded number of queues/threads. Fail closed on inability to allocate workers or admit tenant budgets.
4. The worker loops are `noexcept` and use STL allocation for completion queues: process-wide allocator exhaustion could terminate the host. Production hardening must preallocate fixed ring slots and explicitly handle allocation/fatal-fault containment before multi-tenant exposure. Do not claim per-tenant crash isolation from this stage.
5. No guest ABI, live `.ores` `DbWalActor`, post-resume receipt generation fence, cross-restart deduplication, index state publication, checkpoint and real process/power-loss recovery evidence. No source-org CI step execution proof yet; zero-step is runner admission failure, not native test failure.

Keep draft, preserve no private-source cross-org mirror, independent review and protected-branch merge gates.
