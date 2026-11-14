# P0(DB): LLVM native WAL FD ownership slice

**Status: locally compiled/tested POSIX host primitive, not a compiler builtin, not an async native actor operation, and not a recovered durable database.** Tracks `oreslang-source.llvm#1231`, Java runtime P0 issue #2358 and `oreslang-db#4` / DB PR #11. Contains *fresh* native source-only work in this backend.

## Present

- `WalStorageFdOwner.hpp`: only accepts a preopened, empty, regular FD whose initial filesystem link count is exactly one. Stores a runtime-supplied opaque actor-activation identity *by pointer identity* plus tenant/isolate/shard/actor/generation scope. It does **not** authenticate that pointer or tenant itself; the caller must be a verified isolate runtime issuer, never a guest. No raw path, FD, ticket, receipt or unrestricted native FFI may be sent in actor DATA.
- `write_and_sync_on_disk_worker` is **blocking** and must be invoked **only** from a dedicated bounded disk executor. It validates ODB1 header, lengths, 31-bit IDs, checksum, scope and sequence; limits 4,096 IDs and 1 MiB cumulative accepted WAL bytes; under an exclusive mutex, uses positional `pwrite` with EINTR handling, checks inode metadata and expected file end, then performs actual `fdatasync` (EINTR retry). On any physical error or revocation at the sync boundary, quarantines the owner and throws `UnknownOutcome`, never `Committed`. The host-only `SyncedReceipt` copies the entire bounded frame and owner fields; it does **not** authenticate an Oreslang capability.
- `revoke` is atomic; a physical operation checks revocation after sync. The supervisor must check generation again on receipt handoff. Native destruction and FD release require independently proven completion of all kernel I/O and continuation/actor pins. A destructor is **not** a retirement fence.
- `tests/native/wal_storage_fd_owner_test.cpp`: 20 local host checks covering actual write/fdatasync/reopen, bad checksum, foreign/stale tokens, duplicate ID, sequence gaps, hardlink refusal, read-only FD sync/write failure and quarantine. Includes an independent OS worker execution test. Not an Oreslang compiled guest or power-loss test.
- `scripts/verify-wal-storage-fd-owner.sh`: exact commit/tree and pinned compiler SHA-256, compile `-std=c++17 -Wall -Wextra -Werror`, byte-exact stdout. No host simulator claiming guest results.

## P0 integration absent here

1. Wire the isolate's *real* tenant/actor identity and generation to a nonforgeable capability, with canonical Linux `openat2`/directory-relative constraints and exclusive mount/inode custody. An arbitrary host caller could otherwise create the opaque pointer and supply its own FD: **do not expose this factory to guests**. If the host cannot prove custody, refuse operation.
2. Add bounded disk-executor integration with the existing `AsyncIoOperationLifecycle`, `AsyncIoActorLifetimePin`, retirement and epoch fences. `write_and_sync_on_disk_worker` must **never** run on an actor carrier. Cancellation after I/O dispatch remains `Unknown`; a copied execution claim is not a storage authority.
3. Add streaming, generation-fenced recovery; cross-window durable op ID history; checkpoint temp-sync-atomic rename-dir sync; malicious tail and middle corruption rejection; shutdown with pending syscall and independent healthy-tenant progress.
4. Compile real `.ores` `WalActor` with exact pinned compiler and execute native guest fixtures. Run separate process crash/restart tests before and after append, fsync, index publication and ACK; acknowledge only after authenticated persistent sync and state publication. Native crash tests are **not** simulated by host receipt tests.
5. Require exact-head GitHub CI runs with nonzero executed steps, source+tree+binary hashes and independent review before promotion. This source slice does not prove physical tenant heap separation, host filesystem sandbox, or end-to-end DB durability.

## Hard fail-closed limitations

The primitive accepts an **already authenticated** FD and activation token from a runtime layer not implemented by this PR. It does not provide a guest API, async dispatch, built-in directory sync or process restart. No source mirroring or merge. Keep draft.

## Audit: append-mode descriptor custody (2026-10-31)

The preopened POSIX descriptor must not carry `O_APPEND`. On Linux, `pwrite`
with `O_APPEND` may ignore the passed offset, violating WAL positional-write
invariants. The host owner now rejects `F_GETFL` failure or `O_APPEND` during
adoption, before retaining FD ownership. Negative fixtures verify rejection
and that the caller still owns the denied FD. This guards one descriptor-flag
hazard; it does NOT establish secure root confinement, no-symlink opening,
FD exclusivity, parent-directory fsync, actor authorization, or durability.

An additional *immediately-pre-write* `F_GETFL` check quarantines the WAL
owner with `UnknownOutcome` when `O_APPEND` is enabled after admission (for
example via a duplicated open-file description). The check is not atomic
against a concurrent descriptor-flag mutation. Race-free exclusive physical
FD custody and supervisor confinement remain mandatory.
