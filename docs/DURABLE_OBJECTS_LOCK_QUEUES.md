# Durable lock queues — LLVM host substrate (draft)

This draft is stacked on [LLVM IsolateVM #1158](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1158). The branch reconciles #1158 at commit `601f64c7abb9cd4f68adad9b5652bd5f08639bc1` in an explicit two-parent merge commit, preserving its latest native guest-exception quarantine behavior.

## Implemented host semantics

- One `DurableLockQueues` instance per tenant IsolateVM incarnation. `Ticket` is host-created, contains actor incarnation and lock/sequence/operation fields, and has a private *queue-instance issuer identity*. This prevents an identically numbered ticket from another OresVM root being accepted as a local ticket. It does not authenticate remote network messages.
- Bounded per-lock FIFO, tenant total entries/bytes/live locks/per-lock depth. Scheduler `claim` and `finish` are separate; the same actor incarnation cannot claim heads on two different locks concurrently.
- Actor retirement rejects queued/claimed durable work; tenant quiescence fences new admission/claims and shutdown requires queue and guest-call drainage.
- Synchronous `try_invoke_durable_guest_result` claims a FIFO head, runs an existing **source-compiled** `.ores` pure scalar `tenant_step` through the versioned `ores_guest_entry` ABI, and releases the queue ticket only after the native guest call returns or throws. An active dispatch pin denies concurrent manual `finish_durable` while that guest is physically executing.
- A guest **C++ exception**, which may follow a partial write, quarantines its tenant and quiesces durable queues; a typed overflow status remains a recoverable result.
- Source-generated integration target `native_oresvm_compiled_ores_durable_dispatch` invokes the real `oreslang-llvmc --isolate-entry=tenant_step` lowering and tests 1,000 operations against tenant A and 500 against tenant B with the *same LockId*, then checks typed overflow without partial counter commit.

## Threat model / limitations

- A guest ticket is a trusted host-only value; this is NOT a guest-provided, serializable, network-authenticated capability.
- Compiled durable scalar calls now select host-owned scalar slots for a specific **actor incarnation** within the tenant, while direct legacy guest calls retain tenant-global `guest_globals` and their serialized `guest_turn_mutex`. Actor-specific turn locks permit physical execution overlap for distinct actors and preserve same-actor serialization through queue claim fencing. Independent runnable locks are **not proof of parallel actor execution**. This milestone validates **host-to-source-generated scalar dispatch**, not a general Oreslang actor compiler or `handle_state` / `critical` lowering.
- These actor scalar slots are **not actor-owned native/off-heap heaps or memory arenas**, and do not implement full ownership or borrow checking. No native `define isolated actor` lowering, actor-bound storage, async continuations, or durable restart has been proven. Tenant isolation remains host-logical for trusted native machine code.
- No durable queue persistence, transactional inbox/outbox, actor scheduler-owned asynchronous continuation, post-crash idempotency, OS sandbox, trusted lease authority or distributed durability is provided.
- Native `TrustedInProcess` does not contain arbitrary memory corruption.

## Tests / verification boundaries

New/expanded C++ tests:
1. `native_durable_lock_queues_test.cpp` — cross-VM *same generation* issuer replay, quotas, FIFO, cancellation, actor non-reentrancy.
2. `native_oresvm_durable_queues_test.cpp` — actor retirement, lifecycle, same-generation foreign OresVM ticket, *physically executing* guest cannot be finished externally, shutdown pin, native guest-exception quarantine/drain.
4. The host suite now includes actor-incarnation scalar slots (two actors inside one tenant, distinct state, retirement/recreate resets state) and a two-actor physical execution overlap test with a bounded deadline.
3. `native_oresvm_source_durable_dispatch_test.cpp` — source-generated LLVM AOT scalar dispatch through the durable queue bridge, two tenants, overflow and shutdown.
4. Existing `native_oresvm_guest_call_test.cpp` from latest #1158 retained to ensure native-guest partial-write quarantine is not lost.

The compiler-generated fixture **requires the full LLVM CMake build and Clang**. It is wired into the compiler-enabled CTest target. The standalone OresVM CMake suite executes the first two queue tests without building the LLVM compiler.

**Execution status:** a local C++17 ticket identity/copy smoke succeeded. A separate **C++17 model of the new actor-state/actor-mutex types** passed GCC, Clang and Clang ASan+UBSan. This model does not compile the exact OresVM repository sources, and it cannot certify the integrated code. All new exact-branch full CMake/CTest and compiler-generated source tests are **not yet executed** here. Exact-head Actions workflow [41006466511](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41006466511) (`llvm-smoke`) and [41006466391](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41006466391) (`conformance`) both have **0 recorded steps**, so they are runner-admission evidence, not test results.

Recommended commands on an authorized same-source private runner:

```sh
cmake -S tests/oresvm-standalone -B build/oresvm-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/oresvm-host --parallel 2
ctest --test-dir build/oresvm-host --output-on-failure

# Full LLVM build using the repository's pinned LLVM/Clang toolchain:
cmake -S . -B build/llvm -DBUILD_TESTING=ON
cmake --build build/llvm --parallel 2
ctest --test-dir build/llvm -R "native_oresvm_(compiled_ores_durable_dispatch|source_llvm_aot_context|guest_call|durable_queues)" --output-on-failure
```

No cross-organization private source mirroring is authorized while the [P0 private-source transfer incident](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/22) remains open without independent approval.
