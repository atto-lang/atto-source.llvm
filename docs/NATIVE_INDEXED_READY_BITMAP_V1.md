# Indexed native actor ready bitmap — review contract (October 19, 2026)

**Status:** incremental, draft native runtime scheduling slice, stacked on native carrier locality PR #672. Not a general guest Task scheduler, not one-million-actor support, and not a formal concurrency proof.

## Authority and indexing

The native scheduler maintains two fixed 16-word bitmaps (uint64_t), one each for CONTROL and ordinary DATA. The 1024-actor admission cap is retained. Every bit is a **derived readiness hint** only, never an execution capability, scheduling token, guest reference, or ownership transfer.

A bit for actor A is set only when A is not physically executing and has an accepted pending CONTROL or DATA envelope respectively. The single existing scheduler mutex is the linearization point for *all* updates:
- successful DATA admission calls `refresh_ready_locked` after incrementing pending DATA;
- successful reserved CONTROL or typed failure admission calls it after publishing CONTROL;
- physical callback dispatch first dequeues a pending envelope and enters the generation-matched actor authority, then marks that lane active and refreshes both bitmaps to zero;
- physical callback completion first releases the actor execution authority, clears the active flag, then republishes readiness for remaining accepted envelopes.

Stop or runtime retirement never drops already accepted work; those envelopes drain and only then does `get_done` become true. Snapshot-free bitmap updates run under the mutex; no atomic bitset updates, no per-message heap allocations, no guest memory sharing, no new reclamation path.

## Ordering and fairness

1. Eligible CONTROL is globally prioritized ahead of DATA.
2. DATA local to the last pthread runs preferentially for at most eight consecutive preferred turns.
3. Every subsequent DATA decision scans the global DATA-ready bitmap in rotating actor-slot order.
4. CONTROL and DATA are both selected from their respective nonempty occupancy words and set bits rather than linearly examining all allocated actor slots.

The implementation uses portable checked word boundary masks plus `__builtin_ctzll` on nonzero words, already consistent with this native GCC/Clang runtime. Indexes are bound to [0, actor_count). `active` and execution generation/owner proofs remain authoritative.

This is **indexed ready discovery**, not a lock-free work-stealing deque or O(1) physical handoff guarantee. Under many runnable actors, the preferred-data scan can still touch many set bits. All scheduler state uses a global mutex; locality, multicore scalability, NUMA/SMT, and tail latency need performance benchmarks before large-scale claims.

## Tests

The source-native `tests/native_actor_indexed_ready_test.c` opens 1024 actors, rejects 1025, checks all 16 occupancy words including boundaries 63/64, 127/128, 511/512 and 1023, mixes CONTROL with ordered DATA for four rounds, and checks exactly-once delivery, actor exclusivity, drained messages, stop and destroy. Existing native CONTROL and send/stop races are mandatory regressions.

The test-only private carrier `ores-dummy-org-oreslang-stack/ores-dummy-org-oreslang-stack-infra#23` mirrors **11 exact native Git blobs** (plus existing Java topology slice) and executes GCC, Clang, and Clang ASan/UBSan, not full LLVM CMake and compiler conformance. Do not claim green until the correct head's workflow **completes successfully**.

## Subsequent scaling work

To exceed 1024, replace the fixed bitmap with bounded dynamically sized words and prove overflow-safe allocation, generation-keyed ready entries, tenant/priority budget isolation, memory quotas, actor slot retirement, no ABA, predictable hot reload and a nonblocking admission path. Independently profile global-lock contention and implement NUMA-local shards/work stealing only after deterministic parity and accounting tests.
