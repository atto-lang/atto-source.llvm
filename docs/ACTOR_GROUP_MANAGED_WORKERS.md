# Native ActorGroup managed worker/readiness contract

Companion to [Java ActorGroup](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/main/docs/ACTOR_GROUP_MANAGED_WORKERS.md).

## Java-source parity target

| Source form | Result | Waits for |
| --- | --- | --- |
| `spawn WorkerActor()` | `ActorRef<WorkerActor>` | Nothing; actor startup is scheduled |
| `await spawn WorkerActor()` | `ActorRef<WorkerActor>` | Successful on_start / constructor readiness |
| `await ref.ready` | `void` | Readiness (or startup failure) |
| `await ref.done` | `void` | Finalization (or terminal failure) |

Awaiting a spawn **never** means waiting until the actor exits. A
group must retain worker references until termination; group membership
alone must never confer lifecycle ownership.

## Executable native substrate

`src/main/cpp/oreslang/runtime/ActorGroupLifecycle.hpp` provides a
bounded group-owned table of identity- and generation-checked actor
references. `NativeActorGroup::adopt(...)` returns the reference
immediately; `mark_ready(ref)` settles its one-shot readiness ticket.
`ref.when_ready(callback)` registers before or after settling, and
receives failure if the actor retires before startup completes.
Old references retain their **readiness ticket only** after retirement
without keeping the actor's heap alive. Generation checks reject
stale ref operations after actor ID reuse.

Shutdown hardening: `stop_workers()` preallocates all stop callbacks and
prepares failure results before committing phase changes, so an allocation
failure cannot strand partially stopped workers. `close()` detaches its whole
worker table under the group lock and then invokes all stop callbacks outside
that lock, even when another callback throws. Completion callbacks may reenter
group methods; no actor-group mutex is retained during those notifications.
These are **CONTROL-plane** operations and may allocate; the guarantees do
not imply a zero-allocation hot-path implementation.

`close()` fences new admissions, rejects pending readiness, clears
group-owned refs, and requests stop without holding the group mutex
during supervisor callbacks. `stop_workers()` requests graceful stop
for live workers without waiting for carrier shutdown. The callback
argument must be a host **enqueue-only** scheduler adapter; guest
continuations must never execute directly on an actor completion thread.

`native_actor_group_managed_worker_readiness` is the CTest for startup,
failure, late awaits, generation/identity, group closure, capacity, and
ready-versus-close races.

Validated exact source with GCC and Clang ASan/UBSan (including reentrant
close and throwing callbacks):
https://github.com/athlet-o/athleto-e2e/actions/runs/38093139710 .

## Explicit limitations

This is a host-side C++ runtime primitive, **not working source-level
LLVM `spawn` or `await spawn`**. A central native supervisor must still
enforce **exclusive ActorGroup ownership across distinct native groups**;
the current per-group table does not provide a global ownership registry. The native compiler currently fails
closed on both expressions; CTest fixtures enforce rejection without
emitted LLVM IR. Pending features include actor constructor/source
lowering, continuation frames, typed ActorRef ABI, scheduler-integrated
readiness callbacks, supervised stop/failure propagation, cross-isolate
capability enforcement and guest ActorGroup APIs. This substrate does not
prove untrusted sandboxing, zero allocations on control-plane operations
or integrated zero-GC source execution. It makes no per-actor physical
heap-isolation claims.
