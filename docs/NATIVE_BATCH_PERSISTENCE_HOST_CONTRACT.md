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
