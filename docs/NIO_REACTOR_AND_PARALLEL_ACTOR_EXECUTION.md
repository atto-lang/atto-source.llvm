# Dedicated native NIO reactors, zero-GC and safe parallel actor turns

Status: **host-only credit proof plus architectural contract**. This document does NOT claim integrated source-level NIO, native `await` continuations, parallel source actors or hard-real-time guarantees. Companion Java: `oreslang-source.java/docs/NIO_REACTOR_AND_PARALLEL_ACTOR_EXECUTION.md`.

## Implementation status on this draft branch (October 16)

- `NativeEpollReactorABI.h/.cpp`: narrow C-callable interface to the C++ reactor manager. It conveys only generation-tagged tickets, FD readiness, terminal status and a completion eventfd to host code. It does not hold or invoke an actor callback; the C VM must still connect these records to protected Future/continuation identities. ABI CTest added; complete-source CI has not executed.
- `NativeEpollReactor.hpp`: Linux-only dedicated `epoll_wait` thread with 64 fixed operation slots, generation-tagged one-shot tickets, nonparking `try_watch/try_cancel/try_pop`, eventfd completion signal and terminal records. No guest callback or trace GC in the reactor.
- `NativeEpollReactorManager.hpp`: throughput configuration now starts at zero pthreads with capacity four. The first valid watch lazily starts reactor #0; additional reactors require sustained ready-event processing and pressure. The `preallocated_rt` profile still establishes the requested fixed reactor-thread population before guest work.
- `NativeActorScheduler.c` now has explicit `ores_native_actor_scheduler_prestart_realtime()`, which prestarts all configured carriers before admission and irrevocably disables runtime pthread creation for that scheduler. `NativeOresVM` exposes `open_realtime(config)` and cold-VM `prepare_realtime(vm)`. The original default `open` still creates **zero carrier threads** until runnable work. No change to actor physical single-flight authority.
- CTest registrations cover the kernel reactor, manager and preallocated four-domain VM startup. The standalone epoll/manager tests passed with GCC and Clang ASan+UBSan against identical Git blob files. The full VM CTest, runtime sanitizer suite and native compiler-to-guest integration have **not** yet been validated on an executing CI runner.
- The reactor manager is a host substrate: no production source-level `std/net`, no native guest `await` continuation hookup, no channel/FD capability migration and no certified WCET. A bounded completion queue alone does not establish memory ownership after an actor restart.

## Verified current native foundation

- Merged PR #280 added `NonblockingTcpLinux.hpp`: nonblocking/CLOEXEC TCP socket, `accept4(...SOCK_NONBLOCK)`, `recv/send(...MSG_DONTWAIT)`, one-shot epoll readiness and eventfd wakeup. `EpollReadiness.wait()` parks only if a dedicated reactor thread calls it; the header does NOT create, own, start, scale or retire a reactor thread.
- The current `NativeActorScheduler` starts zero pthread carriers during construction and spawn; first admitted runnable work starts workers lazily. `NativeOresVM` has four scheduler domains. This is a host substrate, not a complete compiler-to-guest `await` implementation.
- The native actor physical lease is single-owner: `ActorLane.active` plus `OresNativeExecutionAuthority.owner/owner_thread/sequence`. Three worker threads may service *different* actors today, never the same actor simultaneously.
- Native host scheduler memory is allocated during open and uses fixed actor/mailbox slots. Heap reclamation is deterministic after quiescence; no tracing GC is instantiated. Proving absence of runtime allocations for every source feature remains separate.
- The host-only `NativeActorTurnCredits.hpp` now provides fixed 1..3 atomic ticket slots. No heap allocation, GC, mutex, waits or callback on the acquisition path. It returns `std::nullopt` immediately on saturation/CAS contention. A stale/double-released ticket cannot retire a reused slot. It is **not integrated** into native actor scheduling; existing exclusive authority remains mandatory.

## Canonical execution architecture

```text
                    Native OresVM
       +-----------------+-------------------+
       |                 |                   |
    MAIN/CONTROL      actor pools        I/O manager
   root/supervisor   shared/private/      |
                     untrusted        0 reactors at cold start
       |                 |             <=4 throughput default
       |                 |                   |
       +-----------------+-------------------+
                         |
           bounded completion / Future event
                         |
            owning continuation scheduler
```

- **Strict NIO**: Only dedicated I/O reactor threads may wait in epoll/kqueue/IOCP/io_uring completion facilities. Actor, CONTROL and ordinary scheduler carriers may not block in I/O, `pthread_cond_wait` for I/O, `future.get`, or `read`/socket waits. Idle workers may park awaiting runnable queue work; that is not I/O waiting.
- Kernel event callbacks may register/rearm, perform *bounded nonblocking* operations, and atomically settle opaque, generation-tagged completion records, but may not run guest code or arbitrary Future subscribers. Dispatch continuation through its owning scheduler/actor mailbox.
- Zero reactor threads at throughput cold start; the first valid readiness watch starts reactor #0. Optional further reactors appear only when measured event-loop CPU/readiness pressure makes it worthwhile. Five or 10,000 outstanding reads do not require proportional threads. Default target maximum four reactors; throughput scheduler domains retain zero resident carriers until runnable turns are admitted. RT mode is the deliberate exception: its fixed carriers/reactors are preallocated before admission.
- No blocking-I/O fallbacks. Unsupported file/console/stdio operations must fail closed until truly nonblocking backends exist; an async-looking API that secretly parks a host worker is not parity.
- All externally visible capacities must be fixed or bounded: registered handles, command ring, event batches, in-flight operations, completion records, pending continuations, failure CONTROL slots and per-actor mailboxes. Reactor operation identities require slot + generation + monotonic ticket to reject fd reuse and late completion.
- The low-level native `EpollReadiness` constructor and `OwnedSocketFd` factory can allocate/throw and perform kernel operations during creation. This is acceptable only in explicitly defined initialization/ordinary throughput paths, not in a certified hard-real-time critical section.

## Actor physical parallelism: opt-in 1..3

Proposed per-actor limit `maxActiveTurns`, default 1, opt-in up to 3 after complete compiler/runtime admission; it is **not** the logically outstanding message count and not ActorGroup's total `maxParallelTurns`.

Converting `NativeActorScheduler` from exclusive lane to a multi-lease actor requires all of:

1. Per-turn (not actor-global) context: mailbox receipt, current actor/message identity, continuation/await state, cancellation, watchdog, arena scratch memory and terminal accounting.
2. Generation-fenced *multiple* independent execution claims replacing one `owner/owner_thread/sequence`. Every physical claim must authenticate its own pthread identity; no stolen/copied token may release another active owner.
3. Actor-owned persistent state is a fixed-key state map; immutable read loans can coexist, while `critical` writes are exclusive. Loans must end before suspension/cancellation. Contention returns a scheduler wakeup, not `pthread_mutex_lock` on a carrier.
4. No untracked cross-turn mutable sharing through pointers, closures, receiver captures, heap aliases, unsafe FFI or leaked state loans. Runtime memory ownership and compiler Sendable checking must agree. Per-turn arenas should be distinct; shared actor allocations require bounded, audited synchronization.
5. The supervisor's CONTROL/panic/stop, code replacement, and actor finalizer path must fence new DATA admission, serialize lifecycle decisions, and retire only after **all** owner claims, I/O generations and continuation leases are quiescent.
6. ActorGroup-wide capacity may further limit a parallel actor. Exceeding either bound is a nonparking enqueue/reject decision, never an additional pthread per queued message.
7. Existing actors retain the single-flight path. Source declarations requesting three turns must fail closed until fully supported; simply using the new standalone credit gate is not permission to bypass `OresNativeExecutionAuthority`.

## Zero GC versus hard real-time

Zero tracing-GC is a native implementation goal/constraint, but it does not alone make the runtime hard-real-time. For an explicit RT profile:

- Preallocate/pin carrier pools, reactor threads, bounded request rings, completion records, actor heap arenas, and reusable turn contexts *before* deadline-governed work. Do not lazily create pthreads or selectors on critical execution paths.
- Avoid unbounded lock acquisition, dynamic allocations, kernel file/network operations with undocumented latency, heap growth, sanitizer overhead and dynamically linked slow paths in the certified timing envelope.
- Reserve CONTROL/PANIC dispatch capacity and prioritize cancellation/stop without starving DATA; measure maximum rather than only mean latency under overload.
- Use nonparking CAS/try-lock admission, fixed upper bounds for per-turn work, audited stack usage and deterministic lifetime reclamation; prove actual WCET and platform-specific scheduler/network guarantees before claiming hard RT.
- Keep an ordinary throughput profile with adaptive thread growth and zero-to-four reactor scaling. **Do not describe this adaptive throughput profile as hard RT.**

## Tests and integration gates

1. Native host CTest of `NativeActorTurnCredits`: 1 and 3 capacity, duplicate/stale ticket denial, close fence, multi-pthread contention and bounded in-flight count; GCC/Clang and ASan/UBSan.
2. Keep `EpollReactorManager` at zero throughput pthreads until the first valid watch, then exactly one reactor absent sustained pressure; validate invalid admission remains at zero, five/10,000 pending network ops do not cause proportional pthread growth, and cancellation/completion remains exactly once.
3. Integrate `AsyncIoCompletionGate`, generation/lifetime-pinned buffers, actor-facing `Future`/select suspensions, and source `std/net` API with real compiled `.ores` TCP echo and error/EOF/cancel tests.
4. Refactor actor scheduler/authority for per-turn slots, default max 1 and independently verified opt-in max 3; cross-check exclusive `critical` writes, supervisor CONTROL ordering and memory ownership; test actual three concurrent callbacks, not just independent credit issuance.
5. Differential Java/LLVM exact-source tests for reactor operation semantics and actor source behavior; stress allocator-failure, fd reuse, cancellation, shutdown and CPU saturation. Include JDK25/Graal Native Image qualification for Java and native Clang/GCC sanitizer suites.

Relevant tracking: LLVM merged PR #280 (kernel primitives), LLVM merged PR #204 (lazy pthread foundation), LLVM open #463 (arena leases), Java open PR #790 (draft selector TCP) and Java open #1045 (ActorGroup-wide parallelism).

No release claim is implied by source comments or standalone host tests.
