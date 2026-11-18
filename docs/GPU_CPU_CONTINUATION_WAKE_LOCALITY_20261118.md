# CPU/GPU continuation wake locality — native host integration (2026-11-18)

**Status: draft native host-only, fake GPU event in tests. Not an executable
source `gpu` keyword, physical GPU, or compiler-backed AttoLang test.**

## Scheduling contract

`gpu { ... }` and `gpu task { ... }` are the canonical source constructs in
`contracts/v2/CPU_GPU_TASK_V2.md`. A compiler may eventually lower them into
device command submission with a GPU event and an actor-owned suspension.
That compiler/backend path is **not** completed by this patch.

- Root/CONTROL coordinator must only signal a dedicated supervisor-owned
  GPU completion executor, not run device driver polling or guest callbacks.
  PRIVATE/SHARED/UNTRUSTED actor pools stay separate; GPU work stays on a
  provider-authenticated device queue. This patch rejects `progress_all()`
  from **any native actor carrier**, including actor CONTROL turns. It does
  **not yet enforce** that an external host thread is an authorized GPU
  completion-pool worker; the embedding VM must provide that boundary.
- The trusted host first calls `bind_native_actor(scheduler, actor_ref)`
  exactly once. Actor generation numbers are **scheduler-local**: the GPU
  bridge rejects tickets from a different actor slot or scheduler even when
  the generation matches. Once bound, the legacy wake-free `submit()` is
  denied; only the authenticated wake path can admit new GPU actor work.
- During the *current authenticated DATA mail*, native code reserves the
  generation-checked scheduler WAKE ticket. The optional
  `GpuActorProviderBridgeV1::submit_with_native_wake` validates the exact
  original DATA-mail issuance and its bound producer/target incarnation.
  It transfers native WAKE escrow **before enqueue** and preserves it
  through unknown/ambiguous submission. A staging failure before provider
  enqueue aborts the transferred WAKE, and a partially prepared but
  unsubmitted host turn is closed rather than stranded.
- The existing supervisor-rooted `GpuActorQuarantineRegistryV1` holds the
  wake while device work may be active. Merely detaching the actor-facing
  bridge does not release GPU memory or the wake ticket.
- Only the **trusted provider's exact terminal event** lets the registry
  publish READY/FAILED/CANCELLED onto the original native actor scheduler.
  Pending, Unknown, or device loss NEVER publishes READY. A definitely
  not-submitted operation publishes FAILED; only a pre-enqueue failure can
  abort a transferred wake without a device event.
- `NativeActorScheduler` takes the WAKE under the existing actor physical
  lease, **not on the GPU driver/polling thread**. Its WAKE-vs-DATA-vs-CONTROL
  priorities and last-carrier soft affinity apply automatically. GPU queue
  locality (VRAM reuse, device/queue affinity) is independent of CPU L1/L2.
- GPU **actor** cancellation, hardware loss, failed kernel, and graceful
  GPU-actor retirement seal the matching native actor incarnation. In-flight
  GPU event authority remains quarantined until real device quiescence; a
  terminal event then consumes the transferred WAKE without calling guest
  code on a stopped actor. The host supervisor receives the typed CONTROL
  notice independent of the actor's DATA/WAKE pool.
- An INVALID native wake after a verified terminal GPU event puts the
  host-provider binding into `WakeQuarantined`, forbidding duplicate host
  event commit and speculative buffer release. This path requires explicit
  supervisor repair; blindly retrying provider completion is unsafe.
- **Important distinction:** `GpuActorProviderBridgeV1::request_close()`
  closes an entire GPU **actor incarnation**, not one `gpu task` Future.
  Non-terminal `gpu task` cancellation/failure must use a separate
  actor-live Future completion path. This host bridge only accepts DATA-mail
  issued wakes; tasks launched from a TASK/WAKE turn need further compiler
  and capability-gated admission. It is not full source `gpu` lowering.

## Verification

`tests/gpu_actor_scheduler_wake_host_test.cpp` links the REAL
`NativeActorScheduler.c` with the existing host registry/provider bridge.
It checks DATA-mail provenance, exact native actor/scheduler binding,
same-generation sibling denial, wake-free bypass denial, actor carrier
ownership, one successful READY WAKE after provider completion, native STOP
on cancellation/failure/device loss, supervisor CONTROL, staging-overflow
rollback, unknown-enqueue reconciliation and repeated-poll idempotence.

Run:
```sh
cmake -S . -B build-gpu-wake -DORESLANG_GPU_ACTOR_HOST_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-gpu-wake -j2
ctest --test-dir build-gpu-wake --output-on-failure -R 'native_gpu_actor_scheduler_wake_host|native_gpu_actor_v1_quarantine'
```
Then run ASan/UBSan and process-fault tests; do not label the mock as device
evidence. Hardware GPU tests require authenticated device identity, driver,
source/build/blob IDs and kernel event proof. Java source `gpu` lowering is
still fail-closed on main, so Java/LLVM GPU guest parity is NOT proven.

### Remaining blockers

The completion executor should be one bounded, supervised GPU host service
per device/queue cohort with short nonblocking poll passes; it must not create
one thread per task, and expensive unknown-enqueue/device-loss recovery must
stay on the existing separate privileged recovery path. Neither the native
actor scheduler nor main CONTROL thread should spin on a device event.
Choose queue locality from authenticated device/tenant/generation identity,
not untrusted guest-provided queue IDs.

Compiler generated `gpu task` HIR/MIR must mint and bind source continuation
and actor incarnation, with resource loans, explicit upload/download, device
quota and on-stop isolation. GPU queue placement should reuse device buffers
within authenticated tenant/device/queue boundaries (no implicit migration).
Measure kernel queue waits, PCIe/UMA transfers, VRAM misses/reuse and CPU
L1/L2 misses around host resumption separately.

**Execution:** commit/test source staged. Exact-head CMake/CTest, sanitizers,
real GPU and compiler-backed `.atto` execution are NOT EXECUTED here.
