# Compact actor scheduling — experimental density slice

**Status:** host-only experiment. Not production runtime, sandbox, compiled `.atto` execution, tenant security proof, or actor-lifecycle parity.

The native `runtime/actor_runtime.c` bootstrap currently assigns a 256 KiB pthread stack and 64 KiB actor arena to each of at most 32 actor instances. That is useful for actual native guest bootstrap tests, but expensive for predominantly idle actors. **Do not silently switch its ABI or change its isolation claims.**

`tests/actor_pool_density.c` independently explores:
- Two shared carrier pthreads, rather than one thread per logical actor.
- A 40-byte control record in the tested x86-64 Linux configuration, and a 4-byte ready-queue slot.
- An actor-owned FIFO mailbox allocated on first send; no mailbox allocation for never-messaged actors.
- A single active turn per actor; messages sent during a turn remain queued until the turn finishes.
- A bounded ready ring and a 16-message-per-actor mailbox cap, with explicit backpressure.
- An opaque 64-bit experiment-only handle containing generation and 1-based slot, rejecting malformed/stale generation numbers.
- Closing admission first, draining accepted turns, then joining the shared carriers. The test checks the final sum after deterministic drain.

## Reproducible host test

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror -pthread tests/actor_pool_density.c -o /tmp/actor_pool_density
/tmp/actor_pool_density
clang -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/actor_pool_density.c -o /tmp/actor_pool_density_asan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/actor_pool_density_asan
```

Local Linux smoke (2026-11-19): GCC and Clang normal builds and Clang ASan/UBSan completed successfully. 100,000 logical actors, two carrier threads, four simultaneous producer threads, 12,000 accepted turns to one hot actor, final sum 12,000. `sizeof(actor_t) == 40` bytes on this target; the actor control table and ready array together account for 4,400,000 bytes of **nominal array storage**, or 44 bytes per logical actor, excluding all OS/runtime/allocator overhead. This is **not** measured process RSS, not memory per active actor, not a latency measurement, and not evidence for one million actors. No comparison to V8, GraalVM, Wasmtime, or independently compiled Atto guests was run. The experimental mailbox allocates 16 × 8 bytes for the one hot actor in addition to allocator overhead.

Unlike `tests/tenant-vm/tenant_vm_test.cpp`, this test does not admit any executable tenant module or exercise per-tenant boundaries. It is the simplest possible shared-carrier density baseline to feed into the real TenantVm/actor integration.

## Required integration before production

1. Keep the current native guest runtime and its evidence intact. Bridge actual compiler-generated `atto_actor_init` and `atto_actor_receive` to carrier-bound actor contexts, not the host-only sum callback.
2. Specify one compatible ActorRef ABI (the current native guest bootstrap uses a 32-bit 16/16 handle; this experiment deliberately uses a **different 64-bit handle**) and proper generation exhaustion/quarantine on actor restart and slot reuse.
3. Move actor state and mailbox allocations to bounded, fault-atomic, actor-owned lazy regions with verified shutdown/borrow/continuation retirement. The legacy [actor-slab draft](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1874) shows why a 4 KiB slab per tiny actor can make memory density **worse**.
4. Add fair scheduling across actors and tenant domains, budgeted reductions, priority supervisor CONTROL lanes, bounded wait queues, cancellation and nonblocking socket reactor integration. A single global mutex is a deliberate prototype limitation; benchmark contention before sharding.
5. Measure RSS/USS, virtual address reservations, thread stacks, peak commit, cold-start time, actor creation and dispatch throughput, p50/p99/p99.9, suspended continuations, 100K/1M idle actors, and workload-matched V8 isolate and pooled Wasm baselines. Take measurements in separate clean processes under fixed toolchains.
6. **Security:** this shared-carrier prototype is for **trusted actors only**. A native SIGSEGV in a carrier can kill the entire process. For mutually untrusted tenants inside one PID, require reviewed sealed-code verification, real linear-memory bounds, CFI, guest prohibition on direct syscalls/FFI, enforced CPU/heap/stack limits, killable tenant compartments, generation fences, and a trusted per-socket network capability broker. Until real hostile compiled `.atto` two-tenant same-PID tests pass, fail closed or use an explicitly sandboxed process.
7. Only mark guest/runtime implementation `passed` after actual compiler-produced `.atto` binary execution on exact source tree and independently checked tests. An Actions job failing before executing steps is not a pass.

Contract and tracking: [Atto spec PR #49](https://github.com/atto-lang/atto-spec/pull/49), [LLVM TenantVM issue #17](https://github.com/atto-lang/atto-source.llvm/issues/17), [Forge TenantVM issue #31](https://github.com/atto-lang/atto-source.forge/issues/31), [native fault hardening PR #47](https://github.com/atto-lang/atto-source.llvm/pull/47).

The scheduling experiment is **independent** of native process-worker hardening PR #47 (different files and trust class) and complementary to the legacy slab allocator draft #1874. It does not supersede or solve either.
