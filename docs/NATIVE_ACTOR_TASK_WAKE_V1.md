# Native actor-owned Task WAKE bridge (host ABI v1)

Status: **working implementation candidate, execution unverified**.
This is an incremental host/runtime bridge, not yet the compiled `.atto`
actor-`task { ... }` language feature or five-backend parity.

## Supported path

An actor's authenticated DATA or READY WAKE callback can start a preallocated
`OresNativeActorTask` record. The bridge reserves a scheduler-issued,
generation-checked WAKE credit for the **same actor incarnation**, transfers
that credit, and publishes initial work to the *existing actor worker pool*.
It never opens a Task executor or borrows supervisor CONTROL. Subsequent
`await` uses `task_suspend` to reserve a bounded continuation credit without
parking the actor worker; an external producer only uses `task_publish` to
schedule a READY/FAILED/CANCELLED event. The guest always runs through the
scheduler's authenticated WAKE callback.

The owner retains a real native actor resource loan. That loan is not
retired in `task_finish`: the scheduler's trusted `after_wake` hook runs
**after the guest callback and its physical execution claim unwind** while
the actor lane is still pinned. Only that hook may acknowledge the physical
retirement receipt; a pending callback cannot manufacture `get_done()`.
A copied WAKE ticket is **not** a physical-retirement capability: the hook
is authenticated against the scheduler's post-dispatch thread-local receipt.
An external Future producer passing the genuine ticket directly to
`ores_native_actor_task_after_wake` must be rejected.

The record is one-shot and requires host-owned, nonzero actor/slot/task/code
generations. Stale foreign producer generations are rejected. No allocations
occur on normal task start/await/resume paths after host initialization.
The scheduler's existing eight WAKE tickets per actor bound outstanding
continuation admission.

## Bounded actor-owned task registry

`NativeActorTaskRegistry.{h,c}` adds a preallocated, fixed-capacity ledger
for the exact native actor incarnation. The current maximum is eight tracked
Task records per actor (the existing preallocated WAKE credit budget), with
explicit unregister required before recycling capacity. Every source Task
must be started through `ores_native_actor_task_registry_start`, which
serializes registration and Task admission against `registry_stop` under
one ledger lock; the scheduler's own retain/WAKE transfer gate independently
arbitrates with external STOP under its scheduler lock.

A nested child must name a currently tracked parent, and that parent must
be executing an authenticated READY WAKE on this actor's physical worker.
A raw Task pointer or foreign thread is insufficient. The registry stores
parent-child edges and provides
`ores_native_actor_task_registry_scope_done(parent)`, which stays false
while ANY registered descendant still has an incomplete physical post-WAKE
retirement receipt, even after the parent's own result has become terminal.
`registry_cancel(parent)` revokes all descendants under the same ledger
lock that serializes new child admission.

The host's standard STOP hook must call `registry_stop`, which seals the
ledger before canceling every registered Task and calling the exact native
scheduler's STOP. `registry_close` fails until the ledger is empty, STOP is
sealed, and the scheduler reports that actor's physical terminal state.
No guest code may bypass this registry and start through the legacy
trusted-host TaskBridge directly. The compiler must eventually enforce that
use of the registry for all actor source Task codegen paths.

## Explicit lifecycle boundary

This bridge is not a complete actor TaskScope. The **new registry supplies
an automatic STOP hook for all registered Tasks**, replacing manual per-task
cancellation when hosts use `registry_stop`. Bare trusted-host Tasks outside
that registry still require manual cancellation before calling raw
`ores_native_actor_scheduler_stop_sealed`.
The host also owns the lifecycle of each external Future subscriber/publisher:
it must revoke and physically join all callbacks that could still touch a
Task record **before** `ores_native_actor_task_close` or freeing that record.
The host-only bridge cannot make an unregistered raw C pointer or a foreign
producer's task argument lifetime-safe. This is a prerequisite for compiler
integration and not yet an automatic `TaskHandle.get_done()` scope join.
`ores_native_actor_task_stop_actor` provides that ordering for a single
Task record. For an actor with several live Tasks, the host/compiler must
first cancel **all** tracked records and then stop the actor. The actor's
resource loans and queued WAKE receipts fence its DONE until callbacks
physically retire. A direct scheduler STOP without task cancellation can
leave a suspended Task's physical loan outstanding indefinitely; it must not
claim successful physical DONE.

Also not yet implemented: compiler lowering of actor task bodies, general
`Future<T>` graph/admission, lexical async defer, parent TaskHandle lifetime
joins, untrusted logical cohort budgets, actor hot-load generation migration,
or full native sealed-state capture checks. These remain fail-closed until
the compiler and supervisor have matching release gates.

## Host-level regression

`tests/native_actor_owned_task_wake_test.c` exercises:
- native actor DATA -> Task WAKE -> nested child WAKE;
- a parent that suspends, then resumes only after a foreign host thread
  publishes the scheduler ticket (producer runs no guest);
- the exact actor physical turn and no CONTROL task admission;
- a deliberately held final guest WAKE demonstrating that physical DONE
  stays false until the callback exits and the trusted post-WAKE hook runs;
- nested parent/child ledger admission, bounded registry closure, and
  CONTROL-plane task admission rejection;
- actor STOP while suspended, automatic cancellation of all registered Tasks,
  cancelled transferred wake and late foreign completion rejection;
- parent physical completion before a suspended child, rejecting parent
  `scope_done` until the child retires, even when the parent's own result
  is terminal; transitive cancellation of that child;
- physical actor `wait_done` fencing before registry destruction.

Run CTest `native_actor_owned_task_wake_host_v1` on an exact source checkout,
or the isolated GitHub Actions source branch workflow
`native-actor-owned-task-wake.yml` with GCC O1, Clang O2 and GCC ASan/UBSan.

**Source org run #43333209930 completed failure before any job steps were
reported**. This does not establish a compilation or execution result.
The LLVM private-CI policy denies cross-repository mirroring without approved
source-main protection, exact destination and pinned PR/test plan; do not
export this branch to an unapproved carrier as a workaround.

Related safety precursor: `oreslang-source.llvm` PR #1715 rejects actor
Task syntax from the legacy synthetic pool. Both changes are necessary but
insufficient for compiled native guest actor-task parity.

## 2026-11-13 safety hardening — physical-return handshake and early-completion escrow

Source actor Tasks now distinguish `RUNNING` -> **`SUSPENDING`** ->
`SUSPENDED` -> `QUEUED`/resume. A guest's `task_suspend` call reserves
a generation-checked wake credit but leaves `in_wake=1`. The only physical
suspension acknowledgement is `after_wake`, invoked by the native scheduler
**after its exact execution claim has unwound**, with no scheduler lock held
and the actor lane still pinned. The external Future producer never executes
guest code or obtains that physical after-WAKE authority.

A Future may complete while the original guest callback is still on-stack.
Dropping that outcome would strand `await` unless the producer retried.
Therefore the bridge accepts **one early READY/CANCELLED/FAILED outcome** into
a fixed-size per-Task escrow under the record mutex. A second competing
completion is rejected. The escrow is **not** a runnable WAKE: only the
post-WAKE callback flushes the outcome to the actor's existing bounded
scheduler queue. If STOP wins while the callback is on-stack, cancellation
clears the escrow, revokes the transferred wake, and leaves the resource loan
pinned until physical callback return. Failure to release/abort a wake
quarantines its loan instead of forging DONE.

An outcome arriving *before Future subscription / task_suspend registration*
is not captured by this bridge. Compiler-generated `await` must atomically
register the Future subscription or inspect already-settled readiness under
the Future's own producer protocol; an unregistered producer cannot blindly
discard a completion. The bridge's `OresNativeActorTask*` is a trusted-host
opaque record, not a guest pointer or public cancellation capability.

The native host integration test deliberately holds the first guest WAKE
callback before its physical return, attempts a wrong-generation completion,
accepts one valid completion into escrow, rejects duplicate completion, then
releases the callback and checks exactly one later actor-owned resume. A
second scenario races STOP with escrow and proves no guest resurrection and
no premature physical DONE. A separate compiler-negative pair guarantees that
a synchronous source actor cannot lower `task { }` to the legacy foreign
Task pool and cannot use `await` without `suspends fnc/mtd` and genuine
actor-owned continuation lowering.

**Execution limits:** native/LLVM CTest and actual compiler guest runs on this
new head still require a runner with positive recorded steps. Static source
and blob integrity checks do not establish C compiler or guest success.
The protected private-source export policy does not yet authorize a
PR-specific alternate-organization carrier plan for #1734; approval is
tracked in issue #1737. No private source was exported.
