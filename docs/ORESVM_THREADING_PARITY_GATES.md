# Java/GraalVM ↔ LLVM OresVM: thread / scheduler / NIO parity gates

This document distinguishes **logical scheduling authority** from **OS threads**.
Four native OresVM execution domains are **MAIN/ROOT CONTROL plus three actor
domains**, not four pools of actor workers. No actor or ActorGroup owns its
own physical pthread.

## Reference behavior (current Java `main` versus native)

| Boundary | Java/GraalVM baseline | LLVM evidence / status |
| --- | --- | --- |
| VM CONTROL bootstrap | `OresVM.startup()` prestarts its configured bounded CONTROL executor before `STARTED` | `ores_native_vm_start_main_control_loop` starts **one** dedicated condition-driven native CONTROL thread before guest actor admission; restricted `SourceActorBootstrap` invokes it |
| ROOT/main guest turns | `OresScheduler` owns ROOT turns; single-flight process refinements remain in draft #519 | Native MAIN actor lane exists; source guest `await`, root process ownership and timer refinements are incomplete |
| Actor carriers | PRIVATE / SHARED / UNTRUSTED dispatchers, created once per runtime | Three native worker domains corresponding to PRIVATE / SHARED / UNTRUSTED; zero started carriers at cold open, growth on actual accepted runnable work |
| CONTROL versus actor execution | No uncontrolled guest execution on a CONTROL/event-producer callback | Master thread calls only `signal_control`; actual mailbox CONTROL handlers run on their owning actor carrier |
| NIO readiness | Java PR #1979 proposes VM-shared lazy readiness; legacy `AsyncNativeIo` uses bounded virtual-thread dispatch and can block a host virtual thread | Host `NativeEpollReactorManager`: 0 throughput reactors until first valid watch, then 1..4; RT profile preallocates configured reactors |
| I/O completion | Future/continuation must resume on the original scheduler, not the reactor | Native records are bounded and have tickets; **compiled guest Future/await and network capability integration not proven** |
| Limits | CPU-quota-aware Java actor defaults, optional physical turn gate | LLVM has opt-in CPU-budgeted carrier sizing and physical turn gate; independent task scheduler coverage is unproven |
| Shutdown | Guard against synchronous close from scheduler or actor turns | Native CONTROL loop is signalled on retirement and joined before pool destruction; previously accepted actor turns drain; external producers must be fenced |

## New native CONTROL loop

`ores_native_vm_open` continues to support legacy **manual host pumping**,
with no eager pthread. Trusted bootstraps opting into the autonomous main
CONTROL service call `ores_native_vm_start_main_control_loop` **before**
spawning actors **and before any real-time preallocation**. This prestarts
exactly one CONTROL worker, rejects late thread provisioning, and leaves all
actor carrier counts at zero until a runnable actor turn is admitted. Once
`prepare_realtime()` has been attempted, success or failure, a new CONTROL
thread is forbidden; hosts needing both services start CONTROL first, then
prestart their RT carriers. The compiled source-actor bootstrap uses the
ordinary non-RT path.

The master CONTROL loop never calls user/guest callbacks, handles NIO
readiness, or performs blocking socket I/O. It consumes coalesced per-domain
wake bits and signals the existing scheduler condition variables. Actual
CONTROL mailbox admission remains a separate, generation-checked operation.
Notification counts are advisory, **not a message count**; multiple wakes
may coalesce. Existing explicit `control_pump` consumers remain valid, but
a host using the autonomous service should not infer delivered messages
from `control_pending` or the dispatched-wake counter.

The main loop and three actor pools are distinct from the NIO reactor.
NIO may have 0..4 dedicated epoll threads in ordinary throughput mode.
This is intentional: a reactor **core service** exists at bootstrap, but
its first physical epoll thread is instantiated on the first valid watch;
only the opt-in RT profile prestarts reactor pthreads.

## Acceptance and proof boundaries

1. **Compile / test**: `native_vm_main_control_bootstrap_and_shutdown`,
   `native_vm_master_control_wake_coalescing`,
   `native_ores_vm_four_carrier_domains`,
   `native_vm_nio_lifecycle` and restricted compiled-source actor tests
   must execute at the same immutable source head. `steps=[]` is
   non-execution, not a pass.
2. **Adversarial lifecycle**: repeated CONTROL start, rejected late start,
   early pending notifications, concurrent notify flood, actor mailbox
   isolation, retirement joining, cold NIO bootstrap exclusion, and
   no guest callback on reactor/master threads. Linux's
   `native_vm_main_control_pthread_failure_retry` uses linker-wrapped
   `pthread_create` to prove EAGAIN leaves a cold, retryable VM with
   pending notifications intact. Exercise RT-first late-start rejection
   and CONTROL-first-then-RT ordering. Run GCC/Clang ASan/UBSan and TSan
   when supported; written tests alone are not proof of execution.
3. **Compiler bridge**: extend beyond the current restricted source actor
   path, making ROOT/actor Future wakeups, timers, guest `await`,
   cancellation, `defer`, actor stop and restart generation-fenced and
   scheduler-owned.
4. **NIO transport**: connect real compiled `.ores` accept/read/write to
   a bounded reactor and its owning actor without ever parking an actor
   carrier on kernel I/O; verify fd reuse, ownership transfer, buffer pins,
   overload, shutdown cancellation and exact-once completion.
5. **Security**: the native UNTRUSTED thread pool is **not** an isolate or
   process sandbox; it must never be presented as a hard security boundary.
6. **Cross-backend parity**: run equivalent guest contract fixtures on
   JVM/Native Image and LLVM, with CPU-quota, 1/2/4+ core, 0/1/many actors,
   lazy/RT startup and shutdown race profiles. Maintain source and
   runtime behavior separately from host C-level unit tests.

**Important:** this change closes one host CONTROL service topology gap.
It does not mean the LLVM source compiler has reached parity with Java's
full VM, nor that strict real-time scheduling or source-level NIO is complete.
