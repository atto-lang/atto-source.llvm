# Durable lock queues — LLVM host substrate (draft)

This addition is stacked on LLVM [tenant isolate PR #1158](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1158); it is **not** an independently mergeable production feature.

Each private noncopyable tenant `IsolateVM` now owns its own `DurableLockQueues` instance with a fresh generation. The `OresVM` authority exposes generation-checked host-only enqueue/release/cancel/snapshot operations, with the queue fenced by isolate lifecycle.

- Queues are FIFO **per numeric LockId**, and separately bounded by live lock count, pending+running entries, entries per lock, and aggregate payload accounting.
- Only an exact head ticket can release. Non-head/foreign/stale tickets cannot release; cancel may remove a waiter only.
- Each distinct lock can be independently runnable. This is *not* evidence of simultaneous safe guest execution: current OresVM uses a single per-tenant guest mutex. An actor still must never execute reentrant turns even across distinct locks.
- Quiescence closes new admissions. Shutdown refuses completion until all durable queue tickets retire; physical callbacks/continuations/IO will need separate scheduler pins.
- The queue is **ephemeral in-memory lock scheduling**, not persisted durable message delivery or transactional data. Host operation IDs deduplicate only simultaneous outstanding operations, not post-crash retries.
- No guest ABI has been exposed, no native guest code receives a C++ pointer, and there is no OS-process isolation. Untrusted native tenants remain prohibited until sandbox workers exist.

Verification: local standalone queue test passed with GCC C++17 and Clang ASan+UBSan; native OresVM queue integration test added to standalone CTest registration, but must be executed in an environment with the complete branch checkout. GitHub Actions source-org jobs had prior zero-step admission failures, which are not compiler test results.

Follow-up: authenticated actor scheduler binding, object-to-lock collision-free mapping, durable journal/transaction driver, migration epochs, persisted alarms, compiled `.ores` ABI, guest caps, limits on all native memory and I/O, real sandbox, conformance tests.
