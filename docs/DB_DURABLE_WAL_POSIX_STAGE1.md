# Native Oreslang DB POSIX WAL physical sync — stage 1

**Host-only POSIX runtime; no guest I/O, commit ACK, authenticated storage
capability, allocator/actor scheduler integration, or end-to-end durability.**

The header-only `DbDurableWalFile` requires an already authorized
directory FD (never a guest-created path), uses `openat` with O_NOFOLLOW
and O_CLOEXEC, rejects hardlinks/nonregular files and takes advisory
exclusive flock, validates generation/operation and 31..1118-byte frames,
then writes and calls fdatasync before returning a metadata-only receipt.
A write/sync error poisons the file handle: the operation is `Unknown`
until WAL bytes are replayed and independently reconciled.

A caller must enforce directory privacy, runtime-issued authority,
tenant/isolate/actor-incarnation permissions, bounded async dispatch,
and native actor resource/continuation lifetime pins; this header does
not itself grant any authority. Advisory file locks cannot exclude a
hostile co-writer. Neither ACK nor ledger settlement is exposed.

POSIX test compiles independently at C++17 and exercises invalid lengths,
generation, competing owner, symlink/path denial, file sync/reopen, and
separate-process `_exit` recovery. It is **NOT** a filesystem power-loss,
compiler-generated guest, sanitizer or actor end-to-end test.

CMake/CTest name: `native_db_durable_wal_host_only`; on-disk reference
evidence must be distinguished from Oreslang guest execution.
Local isolated g++ host smoke completed; exact-tree GitHub CMake/CTest is
**NOT EXECUTED** until actual admitted runners execute steps.

Before DB commit: trusted opaque handle registry, per-call owner fences,
async physical loans, parent directory fsync/rename, WAL replay and
idempotency, partial write/sync fault injection, Java/Graal differential
fixtures and real process-crash tests. No private source transferred.


## Stage 1.1: native actor owner-retirement model

The host-only `DbWalOwnerRetirementGate` binds the physical admission
lifecycle to one actor ID and generation. The one-way sequence is
`open -> draining -> closing -> closed`, with an in-flight worker pin
preventing close and an irrevocable drain preventing fresh writes.
An owner may retry close after the actual physical worker retires;
stale owner/generation and duplicate close are rejected. This is an
independent state model, NOT YET integrated with
`AsyncIoActorLifetimePin`, WAL fd close, or compiler-generated guest.

CTEST: `native_db_wal_owner_physical_retirement_host_only`
(build target `oreslang-native-db-wal-owner-retirement-test`).
Local isolated g++17 500-iteration concurrent model test passed under
`-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined`
using a matching local source copy; the exact-head CMake/CTest runner
remains NOT_EXECUTED. Real OS resource lifetime and fdatasync/ACK
semantics need additional integration tests.

## Stage 1.2: native physical actor lifetime integration

The host-only `DbWalPhysicalRetirement` bridge ties an admitted WAL
gate to the existing `AsyncIoOperationLifecycle` and
`AsyncIoActorLifetimePin`. It validates actor ID and the full
generation *before* allowing physical-loan release (including rejecting a
generation not representable by the WAL gate). A cancellation/timeout and
unregister alone are insufficient. Only after
`ready_to_reclaim()` succeeds may the bridge release the ActorRef
resource loan and retire the WAL writer slot. A duplicate release
fails closed; a mismatched incarnation is rejected in the constructor.

`tests/native_db_wal_physical_pin_integration_test.cpp` uses a real
`NativeActorGroup`, resource registry, actor retirement barrier and
`get_done()` gate to assert that drain/close and actor retirement are
blocked until the callback, kernel-registration and physical pin are
retired. Registered CTest target:
`native_db_wal_physical_pin_integration_host_only`.
The test is authoring evidence until a real exact-head CMake/CTest
runner executes it. This bridge is not yet called by a production WAL
actor or guest I/O lowering; it has no physical descriptor ownership,
sync receipt, commit ACK or crash/replay path.
