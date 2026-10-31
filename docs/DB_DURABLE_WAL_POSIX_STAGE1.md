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

## Stage 1.4 — bounded native shard authority

The new host-only `DbWalShardGrantRegistry<Capacity>` defaults to a
32-slot fixed `std::array` and performs no per-grant heap/GC allocation.
It is a CONTROL-plane mutex-protected model, **NOT** a hard-real-time
admission primitive. A grant is move-only and bound to one registry
identity, context number, tenant/shard, actor ID, actor generation,
monotonic slot epoch, exact fstat device/inode tuple, and the previously
bound `DbWalOwnerRetirementGate` object. One shard and one physical file
identity can each have at most one active/draining/closing grant. A stale
epoch, incorrect registry/context/actor/generation/file, or a sealed
registry fails closed. A shard remains reserved while draining or
closing. Retirement requires the *same* bound gate to report closed
with no active operations; a forged/mismatched different gate is rejected.
Epoch exhaustion permanently fences new issuance instead of wrapping.

`tests/native_db_wal_shard_grant_registry_test.cpp` includes capacity,
cross-context, stale epoch, duplicate shard/file, wrong tenant/actor,
generation mismatch, concurrent drain, premature close, physical-gate
identity, and context-seal checks. Registered as CTest
`native_db_wal_shard_grant_host_only` (build target
`oreslang-native-db-wal-shard-grant-test`).

This is a **host control-plane model only**. Issue parameters are NOT
authenticated, nor is the gate a cryptographic/OS-issued file handle;
the trusted runtime must validate actor identity, tenant authority,
actual fstat values and the physical descriptor before calling issue().
No Oreslang guest exposure, async file dispatch, kernel proof of close,
ACK, persistence/replay or Java differential execution is provided.
Exact-head CMake/CTest and sanitizer tests are **NOT EXECUTED**.

## Stage 1.5 — atomic WAL admission versus drain (locally executed)

`DbWalShardGrantRegistry::can_write` is now explicitly a **diagnostic
observation**, never authority to dispatch a write. Two separate calls
(`can_write`, then `DbWalOwnerRetirementGate::admit`) could have allowed
a drain to interleave. New `begin_write` checks the context, shard,
tenant, actor, generation, file identity, registry epoch and ACTIVE state
under the registry lock, then atomically acquires the bound physical
gate's active slot before releasing that lock. `drain` uses the same
lock order, making write versus drain linearizable. The trusted physical
worker may release a DRAINING in-flight write with
`physical_write_retired`, without confusing logical Future completion
with physical I/O retirement. Move-assignment of `Grant` is prohibited
to avoid silently orphaning a live slot.

The tracked `native_db_wal_shard_grant_registry_test.cpp` includes
positive and negative admission, stale actor/generation, capacity,
duplicate shard/file, physical-retirement and 2-thread drain/write tests.

**New local evidence (NOT hosted or guest):** source SHA
`d03a40dc7549ffce0d30a5090f53e0e315c43c5a`.
Manually reconstructed source files were checked using `git hash-object`
against their *exact GitHub blob SHA*:
`DbWalOwnerRetirementGate.hpp` `c041c5ffd177688d04f5248c3d8ecd98552032f4`,
`DbWalShardGrantRegistry.hpp` `80ea864efef1d3d627ef1f35143e569e30279cff`,
`native_db_wal_shard_grant_registry_test.cpp` `2f71d647cb9ca8fea98ad1e52b40c0a5c80e8646`.
The **tracked exact-blob test passed** locally with
`g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread
-fsanitize=address,undefined -fno-omit-frame-pointer`
and `ASAN_OPTIONS=detect_leaks=1`,
`UBSAN_OPTIONS=halt_on_error=1`.
A separate host harness passed 1,000 concurrent drain/write/close rounds
under the same sanitizers. These prove only the isolated host registry
subset; full exact-tree CMake, TSan, compiler-generated guest, and
disk/restart proof remain NOT_EXECUTED.

Runner-capacity searches on 31 October used **source-free** canaries
in private `akrion-sim/akrion-sim-sync` and
`agent-pontifex/agent-pontifex-docs`; both jobs failed before
runner assignment (`runner_id=0`, zero steps). Recent `voxletra-e2e`
and `scintilla-run-e2e` runs likewise had zero steps. No private
Oreslang source was transferred to an unverified runner.
