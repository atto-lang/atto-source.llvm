# Compact actor scheduling — experimental density slice

**Status:** host-only experiment. Not production runtime, sandbox, compiled `.atto` execution, tenant security proof, or actor-lifecycle parity.

The native `runtime/actor_runtime.c` bootstrap currently assigns a 256 KiB pthread stack and 64 KiB actor arena to each of at most 32 actor instances. That is useful for actual native guest bootstrap tests, but expensive for predominantly idle actors. **Do not silently switch its ABI or change its isolation claims.**

`tests/actor_pool_density.c` independently explores:
- Two shared carrier pthreads, rather than one thread per logical actor.
- A 40-byte control record in the tested x86-64 Linux configuration, and a 4-byte ready-queue slot.
- An actor-owned FIFO mailbox allocated on first send; no mailbox allocation for never-messaged actors.
- A single active turn per actor; messages sent during a turn remain queued until the turn finishes, tested with deterministic mailbox saturation and a FIFO sequence hash.
- A bounded ready ring and a 16-message-per-actor mailbox cap, with explicit backpressure.
- An opaque 64-bit experiment-only handle containing generation and 1-based slot; quiescent actor retirement increments generation and recycles a freed slot without allocating another control record. Old references are rejected after **actual slot reuse**, not only after flipping a test bit.
- Generation exhaustion is fail-closed: a slot at `UINT32_MAX` is quarantined forever rather than wrapping. A synthetic last-generation fixture checks this path; billions of real churn cycles are not executed.
- Failure-atomic carrier-pool startup when the first or second worker cannot start: successfully started carriers are awakened, joined, and all allocated pool resources are freed.
- Closing admission first, draining accepted turns, then joining the shared carriers. A concurrent-producer shutdown test checks exact accepted-message accounting while shutdown races senders.
- A synthetic first-allocation failure leaves no orphaned mailbox entry, and a second attempt can succeed; 1,024 other actors test fan-out and queue correctness.
- **Builds with `-DNDEBUG` retain every pthread/atomic operation**, unlike the previous prototype that called synchronization functions only inside `assert(...)` expressions.

## Reproducible host test

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror -pthread tests/actor_pool_density.c -o /tmp/actor_pool_density
/tmp/actor_pool_density
clang -std=c11 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread tests/actor_pool_density.c -o /tmp/actor_pool_density_release
/tmp/actor_pool_density_release
clang -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/actor_pool_density.c -o /tmp/actor_pool_density_asan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/actor_pool_density_asan
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=thread tests/actor_pool_density.c -o /tmp/actor_pool_density_tsan
TSAN_OPTIONS=halt_on_error=1 /tmp/actor_pool_density_tsan
```

Local Linux hardening verification (2026-11-19): **the exact source blob** `5edceafd91b997eeb9cc95627d3d9653761bc269` compiled and executed on Linux x86-64 with GCC 14.2 and Clang 17: each compiler passed normal `-O2`, release `-O2 -DNDEBUG`, and ASan+UBSan `-O1 -g` runs, and GCC ThreadSanitizer passed. All source test invocations include real assertions via `FAIL_IF_FALSE` regardless of `NDEBUG`. The host fixture creates 100,000 logical actor records and two carriers; four simultaneous producer threads admit and verify 12,000 turns to one hot actor, 1,024 other actors each complete a message, a second actor verifies exact FIFO ordering while paused and saturated, and four senders race a second pool's shutdown with exact accepted/processed counts. A one-time failed mailbox allocation leaves the queue unchanged. `sizeof(actor_t) == 40` bytes on this particular host; actor control records and the ready array together occupy 4,400,000 bytes of **nominal array storage**, or 44 bytes/record, excluding OS thread stacks, process RSS, allocation metadata, active mailboxes and actor state. This is **not** measured RSS or a V8/Graal/Wasmtime comparison. No guest code was compiled or executed.

Unlike `tests/tenant-vm/tenant_vm_test.cpp`, this test does not admit any executable tenant module or exercise per-tenant boundaries. The pause and forced allocation-failure controls are synthetic *test-only* hooks, not API surface; a paused host callback can occupy one carrier indefinitely. Actor slots can now be retired **only when no carrier is executing them and no accepted messages remain**, then safely reused within the *same pool*. Retirement returns busy instead of cancelling queued work; generation wrap quarantines the slot. This does **not** implement an actor supervisor restart protocol, cancellation, cross-pool/tenant identity, memory/borrow retirement, or durable actor state. The current 64-bit experiment-only ref can alias across separately created/reinitialized pools because it has no pool/tenant incarnation field; production capability admission MUST include an authenticated domain/generation and reject cross-pool stale references. It is the simplest possible shared-carrier density baseline to feed into the real TenantVm/actor integration.

## Required integration before production

1. Keep the current native guest runtime and its evidence intact. Bridge actual compiler-generated `atto_actor_init` and `atto_actor_receive` to carrier-bound actor contexts, not the host-only sum callback.
2. Specify one compatible ActorRef ABI (the current native guest bootstrap uses a 32-bit 16/16 handle; this experiment deliberately uses a **different 64-bit handle**) with globally/domain-scoped identity. The test's per-pool generation fencing and exhaustion quarantine do **not** prevent cross-pool stale-handle aliases and cannot be reused as a production capability system.
3. Move actor state and mailbox allocations to bounded, fault-atomic, actor-owned lazy regions with verified shutdown/borrow/continuation retirement. The legacy [actor-slab draft](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1874) shows why a 4 KiB slab per tiny actor can make memory density **worse**.
4. Add measured fair scheduling across actors and tenant domains, budgeted reductions, priority supervisor CONTROL lanes, bounded wait queues, cancellation and nonblocking socket reactor integration. The fixture requeues each actor after one turn but has **no tenant budget** and no guarantee of preemption if a callback blocks. A single global mutex and allocation-under-lock are deliberate prototype limitations; benchmark contention before sharding.
5. Measure RSS/USS, virtual address reservations, thread stacks, peak commit, cold-start time, actor creation and dispatch throughput, p50/p99/p99.9, suspended continuations, 100K/1M idle actors, and workload-matched V8 isolate and pooled Wasm baselines. Take measurements in separate clean processes under fixed toolchains.
6. **Security:** this shared-carrier prototype is for **trusted actors only**. A native SIGSEGV in a carrier can kill the entire process. For mutually untrusted tenants inside one PID, require reviewed sealed-code verification, real linear-memory bounds, CFI, guest prohibition on direct syscalls/FFI, enforced CPU/heap/stack limits, killable tenant compartments, generation fences, and a trusted per-socket network capability broker. Until real hostile compiled `.atto` two-tenant same-PID tests pass, fail closed or use an explicitly sandboxed process.
7. Only mark guest/runtime implementation `passed` after actual compiler-produced `.atto` binary execution on exact source tree and independently checked tests. An Actions job failing before executing steps is not a pass.

Contract and tracking: [Atto spec PR #49](https://github.com/atto-lang/atto-spec/pull/49), [LLVM TenantVM issue #17](https://github.com/atto-lang/atto-source.llvm/issues/17), [Forge TenantVM issue #31](https://github.com/atto-lang/atto-source.forge/issues/31), [native fault hardening PR #47](https://github.com/atto-lang/atto-source.llvm/pull/47).

The scheduling experiment is **independent** of native process-worker hardening PR #47 (different files and trust class) and complementary to the legacy slab allocator draft #1874. It does not supersede or solve either.

## Audit status

- **Fixed (host experiment):** `assert()`-elided mutexes/waits/joins and turn-lease updates under `-DNDEBUG`, a concurrently read non-atomic test pause selector, missing explicit FIFO/backpressure tests, sparse actor fanout and close-versus-send race accounting.
- **Local executed checks:** GCC and Clang, debug/release, ASan+UBSan; GCC ThreadSanitizer. This verifies the host-only test binary, not compiler-generated Atto actors. The GitHub Actions workflow now requires actual execution of normal/release/sanitizer/TSan configurations before CI can be called green.
- **Not implemented:** production code replacement, stackless await/continuations, wake/reactor integration, full actor supervisor restart and cross-pool identity, cross-tenant quotas, native fault containment, secure same-PID tenant compartments, and brokered socket rights. Preserve the source PR as **Draft** until genuine compiler integration and independent executable CI evidence exist.
- **Exact-head GitHub Actions:** must be inspected independently; failure to allocate a runner with zero steps is **not a test pass or a test failure of the C implementation**.

## Second-pass audit: lifecycle and partial-startup failure (2026-11-19)

The exact tested `tests/actor_pool_density.c` Git blob is `9ad537c18541db063ba19230d5d6f76e07550799`. Linux x86-64 GCC 14.2 and Clang 17 each compiled and ran ordinary `-O2`, `-O2 -DNDEBUG`, and ASan+UBSan `-O1 -g` builds; GCC ThreadSanitizer also passed. Repetition on the exact file: GCC release 12/12, Clang release 6/6, GCC TSan 5/5 and Clang ASan+UBSan 4/4, additional to the single initial build runs. All executions are **host-only**; none compile or run `.atto` guests.

New tests check real old-ref rejection after quiescent retirement and reuse, no recycling of a running/pending actor, reset of recycled actor state, generation-max quarantine (test-injected), and partial worker startup failure both before and after one carrier starts. The data structure still measures 40 B/actor on this machine; its free list reuses the retired actor's FIFO-head field and adds a single pool-level head word. It is not a precise resident-memory budget or a full idle actor RSS measurement.

**Explicit unresolved safety boundaries:** a caller must own the pool lifetime and join external producers before freeing it; the test does not provide concurrent arbitrary pool deletion, cross-pool/tenant-unique handles, supervisors/CONTROL, protection from arbitrary native faults, or trusted preemption. Retirement is cooperative and nonblocking: it refuses to retire active/pending actors and cannot interrupt an infinite loop. All three remain required before production integration.
