# Native batch persistence contract (host-only)

Proposed shared semantics: https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/227 and \`docs/PERSISTENCE_SIDECARS_RFC.md\` in the conformance repository.

The new \`NativeBatchPersistence.hpp\` and \`native_batch_persistence_test.cpp\` provide a C++17 **host-only in-memory model** of tenant/actor/state identity, operation-ID dedupe, provider-side activation epoch fencing, CAS versions, bounded owned envelopes and 1,000 distinct actors flushed as four batches of at most 256. It is not LLVM IR lowering, native .ores guest execution, an asynchronous I/O worker, a durable database provider, a VM-isolate or a secure sandbox.

- \`BatchSidecar::enqueue\` copies owned bytes, rejects another tenant and enforces nonzero entry/byte credit even for an empty payload.
- \`flush\` is a synchronous host-model operation and must NOT run on an Oreslang actor carrier. A real native async sidecar should be scheduled on bounded I/O workers with suspended source continuations.
- Mock store \`Receipt::applied()\` is distinct from \`durable()\`. The in-memory mock never sets \`provider_confirmed_durable\`.
- Capacity credits remain charged across the provider commit, including in-flight batches, rather than being prematurely released at dequeue.
- Fencing and version checks run at the provider commit boundary; a directory lease or enqueue-only gate does not protect effects.
- Full production provider must use transactional state+dedupe+epoch validation, durable ACKs, crash recovery, schema migration and authenticated tenant routing. Current host mock cannot prove any of that.

Local test command (run with GCC/Clang, optional ASan/UBSan):
\`\`\`sh
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc/main/cpp tests/native_batch_persistence_test.cpp -o /tmp/ores-sidecar-test
ASAN_OPTIONS=detect_leaks=1 /tmp/ores-sidecar-test
\`\`\`

Actual tested locally with compiler instrumentation; eight host-only test groups passed. The CMake target \`native_batch_persistence_host_only_contract\` is deliberately named not to masquerade as native Oreslang source actor parity.

Missing: compiler emitted actor turns, native scheduler source continuation wiring, hard tenant VM-isolate isolation, per-actor arena address token checks, durable WAL provider, tests under actual Oreslang guest execution and distributed failover.

## Security audit (2026-10-29)

- Provider checks activation epoch before consulting durable-operation dedupe records, so a retired activation cannot replay a previous success.
- Identity validation rejects ASCII control characters (including NUL) across tenant, actor, state and operation IDs.
- The `flush_mu_` guard serializes concurrent flushers around dequeuing AND provider commit, preserving per-actor FIFO/CAS ordering. Quotas remain held while a write is in-flight.
- Local GCC 17 C++ host tests: **8/8**, AddressSanitizer + UndefinedBehaviorSanitizer; independent Clang++ compiler run: **8/8**. This is not guest/native actor source execution.
- The dedicated LLVM PR GitHub Actions job currently has zero recorded steps; CI confirmation is NOT_EXECUTED.


## Real SQLite WAL provider — 2026-10-29

`SqliteBatchPersistence.hpp` is a native C++17, `sqlite3`-linked on-disk provider implementing the same `Store` interface used by the bounded `BatchSidecar`. The provider's SQLite transaction includes activation epoch checks, persisted operation-ID deduplication, per-state CAS, snapshots and versioned events. `Receipt::durable()` is true only after a successful WAL/FULL provider commit, distinct from the volatile `MemoryStore`.

The recovery API returns the latest snapshot and subsequent journal events. For an event-first actor the snapshot is empty and snapshot_version=0. Path selection and epoch grants are **trusted host-only capabilities**; neither is an untrusted guest API. SQLite calls block and must run on dedicated I/O workers, not native actor scheduler carriers.

**Executed local evidence:** GCC C++17 `-Wall -Wextra -Werror -pedantic -fsanitize=address,undefined`, plus an independent Clang C++17 build, successfully executed an on-disk 1,000-actor/4-batch test and reopening/restoration test. A child process performed a confirmed commit then exited via `_exit` while another write remained only in memory; the parent observed the committed state and absence of the unflushed write. Ten repeat sanitizer executions passed before the event-first fix, and new event-first and receipt-check tests also passed. This is actual storage-provider evidence, **NOT compiler-generated `.ores` actor execution or power-loss certification**.

The native provider also checks provider receipt cardinality/identity and caps direct writes at 1MiB. On ambiguous SQLite COMMIT exceptions callers must use the same operation ID for retries; no exactly-once external effects are promised. `CMakeLists.txt` has a dedicated host-only CTest target. The native host Actions workflow now adds real SQLite GCC sanitizer and Clang checks, but its source-org runner admission must be proven separately.

Production blockers: secure tenant/activation directory, async source actor storage bridge, per-actor arena encoding, transaction I/O deadline and fair scheduling, CRC/bitflip storage injection, real power-failure recovery certification, verified native compiler guest execution, and backend conformance.


## Authentic native ActorOwnedHeap + SQLite reactivation integration

Added `tests/native_actor_arena_persistence_reactivation_test.cpp`, included in CMake/CTest and the native host CI workflow. This instantiates the repo's real `ActorOwnedHeap<256,4>` using `ActorHeapIdentity`, writes a POD state while the first heap owns it, serializes scalar bytes in a portable big-endian format, commits through the real on-disk `SqliteStore`, retires the first heap, reopens SQLite, advances the trusted activation epoch, and restores into a new heap generation. It checks that a stale heap handle does not resolve in the new heap, another actor heap cannot read its handle, and an old epoch cannot replay a successful operation.

**Status NOT_EXECUTED on exact repo source:** the native integration test is committed and registered; a full actor-heap header build against the repo has not run because source-org Actions still fails at zero recorded steps and the local artifact snapshot lacks the native heap headers. The previously executed SQLite provider tests remain distinct evidence. This is a native runtime component integration test, **not** compiler-generated guest `.ores` actor execution or VM-isolate isolation.


## Versioned native logical-command SQLite v2 (November 2026)

This **host-only** C++17 slice is added alongside the existing `NativeBatchPersistence.hpp`/`SqliteBatchPersistence.hpp` legacy single-write provider, not as a breaking replacement. `SqliteCommandPersistenceV2.hpp` implements owned byte-vector commands with one or more events, a trusted SQLite WAL/FULL store, stream-head CAS, a stable durable command receipt and result, and exact canonical command-byte deduplication across activation epochs.

All multi-event appends, result bytes, head versions and operation receipts are committed in one SQLite `BEGIN IMMEDIATE` transaction. Epoch authority is enforced before any receipt lookup, including retries, and `verify`/`load_fenced` check the epoch inside short IMMEDIATE transactions. Existing `fences` is shared; versioned `native_command_v2_*` tables are separate from legacy tables. Receipts reject routine UPDATE/DELETE with SQLite triggers. Fenced replay checks contiguous version order, command-receipt coverage and a 65,536-event bound; unsupported indexed compaction fails closed by omission (no compaction API is exposed).

**Executable host test:** `tests/native_command_persistence_v2_test.cpp` runs actual on-disk SQLite, two-event commits, version conflicts, stale epochs, changed-command ID conflicts, commit/reopen/epoch-3 dedup, a trigger-injected second-event rollback, and actual OS `SIGKILL` immediately after a child SQLite COMMIT but before acknowledgment. Local compilation and execution completed successfully with **GCC 14 ASan+UBSan** and **Clang 17 ASan+UBSan**. Exact new local source/test blobs were checked against the newly created GitHub blob objects. The native host workflow now includes both independent configurations. This is **not** compiler-generated AttoLang guest output, an actor scheduler or an asynchronous nonblocking carrier.

**Known parity gap:** the native prototype conservatively accepts printable ASCII identities only. Java v2 accepts NFC UTF-8. This must be resolved through a shared canonical actor-ID serializer before declaring byte-level five-backend parity. Native v2 currently provides synchronous trusted-worker SQLite calls only; it must be integrated with a bounded nonblocking actor-lane completion bridge, snapshot/compaction, receipt archival policy and actual compiler lowering. Do not treat local host tests as a GitHub Actions test pass or hard-real-time certification. Keep draft/unmerged.
