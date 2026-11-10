# LLVM native event-sourced actor host crash/recovery v1

**Status: DRAFT, host-only test code; not executed at this exact head.**

Stacked on native persistence sidecar
[#1180](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1180).

The new test `native_event_sourced_fenced_recovery_test.cpp` performs a
real fork/_exit after SQLite WAL/FULL commits one event and before a caller
reply. The host process reopens provider storage with a fresh activation
epoch, replays the one contiguous committed event into a freshly allocated
`ActorOwnedHeap`, verifies a balance of 42, and checks stale-epoch writes and
cross-epoch replays cannot silently append a duplicate.

The CMake and GitHub Actions host workflows add a narrow test target, with
GCC ASan/UBSan and independent Clang builds.

**Not established:**
- The actor and reducer in this fixture are C++ host code, **not** compiled
  AttoLang `.atto` actor source. No compiler sealed state, pure reducer effect
  checker or asynchronous native carrier integration is proven.
- The current provider reports an operation conflict on same operation ID
  after a new epoch. This avoids double writes but does not return an original
  durable success reply; a trusted cross-epoch operation resolution API and
  durable inbox are still required.
- One event per command is the current sidecar v1 contract. Atomic multiple
  events plus transactional outbox are pending.
- No real source-org Actions test logs yet; a zero-step run is not proof of
  native execution. No private source should be exported without the
  mandatory pre-copy authorization publisher.

**Next:** match Java host bridge event callbacks, then make real compiler
source `EventSourcedActor` state/reducer lowering execute identical fixtures
and register Java/LLVM/.NET/Wasm/Forge evidence under conformance.

## Read-only proof of prior event across activation epochs

`SqliteStore::resolve_committed_event_fenced` now mirrors Java's
`SqlitePersistenceStore.resolveCommittedEventFenced` for an event whose
originating actor activation has been replaced. The trusted host must supply
a verified current epoch and canonical actor identity; the provider checks
the durable operation ledger and the retained event journal row in one
read transaction. Missing operation returns `nullopt`; a stale epoch or
mismatched event fails closed. This does not append an event, rotate an epoch,
or return the old command's successful reply.

**Caution:** Stored event bytes and operation ID are insufficient to prove
the original command's payload or reply. Reconciled events MUST NOT turn
into success acknowledgments until the durable inbox and command fingerprint
are persisted atomically. The test includes wrong-event and stale-epoch negatives.

No GCC/Clang/ASan test execution is claimed until an exact-head runner
actually executes and records nonzero steps.
