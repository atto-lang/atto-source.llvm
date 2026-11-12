# AttoLang native VM actor TASK + WAKE integration (2026-11-12)

**Status: trusted native-host integration patch, tests authored, NOT EXECUTED on this new head. It does NOT compile actor `.atto task { }` or `suspends/await`.**

Stacked on LLVM actor TASK scheduler PR #1724 (which is stacked on #1715). The native scheduler already has a bounded per-actor TASK queue, DATA/TASK/WAKE physical execution ownership, and STOP cancellation callbacks. The VM previously exposed only mailbox and WAKE interfaces, so compiler/runtime clients could not safely access this TASK lane without escaping VM domain ownership.

## Implementation

- `OresNativeVMTaskConfig` is a distinct opt-in configuration object, preserving the binary layout of `OresNativeVMDomainConfig`. Opt-in constructors pair `on_task` and `on_task_cancel` for each domain. Legacy VM constructors remain task-free.
- `ores_native_vm_try_schedule_task(vm, domain, exact_ref, handle)` forwards to the actual domain's NativeActorScheduler. No root Task pool, new pthread, unrestricted host Future producer, or implicit cross-domain fallback exists.
- `ores_native_vm_open_with_wakes_and_tasks` combines exact domain WAKE and TASK callbacks **on the same scheduler**. Only a physical owning actor WAKE turn may schedule a resumed task; the external producer only transfers/publishes the sealed ticket.
- Partial callback pairing makes VM creation fail closed. A zero or foreign handle cannot become valid, and the scheduler verifies the physical lease and exact incarnation on every admission.
- STOP prevents new task admission and routes previously queued handles to cleanup-only callbacks. A running task remains a physical DONE barrier until the real native callback returns. No guest closure is executed by the host-only tests.

## Executable host integration fixture

`tests/native_vm_actor_task_bridge_test.c` uses the real four-domain NativeOresVM and NativeActorScheduler:

1. MAIN runs a TASK callback while STOP seals, asserting `poll_done` remains false until the physical callback exits.
2. SHARED queues a TASK behind a blocked DATA callback. STOP wins; the handle reaches `on_task_cancel`, not guest `on_task`.
3. TRUSTED_PRIVATE admits a nested TASK within the same actor and a second task via authenticated `WAKE -> TASK`. A forged/replayed WAKE ticket is denied.
4. UNTRUSTED, which has no TASK callback configured, rejects task admission instead of silently using another domain's pool.
5. Root caller, foreign ActorRef, zero handles, invalid domain and incomplete callback pairs fail closed.

An LLVM-independent CMake host gate runs the full **native C implementation** without needing llvm-config:

```sh
cmake -S . -B build-actor-tasks -DORESLANG_PROXY_SLAB_HOST_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-actor-tasks --target oreslang-native-vm-actor-task-bridge-test --parallel 2
ctest --test-dir build-actor-tasks -R '^native_vm_actor_task_bridge_host_only$' --output-on-failure
```

Also run the native scheduler TASK test and the complete compiler/guest tests (including GCC, Clang, ASan/UBSan). Require exact-head commit/tree receipts and **nonzero actually executed steps**. Zero-step GitHub Actions jobs are runner-admission failures, not test regressions.

## Compiler-backed parity gate — NOT IMPLEMENTED HERE

The current LLVM actor frontend handles only synchronous `on_message(int)`, not Java's `receive(ActorMail<T>)` plus `suspends/await`. The next PR must add a typed source TaskExpr and a compiler-generated actor-owned continuation frame with checked scalar captures, parent cancellation, frame ownership, generation fencing and cheap cooperative checkpoints. The native task slot takes a **runtime-minted opaque ID**, not an arbitrary guest pointer, and can be resumed only after a validated WAKE outcome. Actor result settlement must remain separate from physical child quiescence.

Required final acceptance: compile one unchanged `.atto` fixture across Java, LLVM, Rust and Wasm; run real guest tasks; prove same-actor nonreentrancy, cross-actor overlap, cross-worker carrier handoff, STOP/cancellation/late completion, and independent review. This bridge alone does not meet that claim.

Security: private source must not be mirrored into unapproved organizations; require source-side approval, independent security review, and exact byte/tree identities for any authorized carrier.
