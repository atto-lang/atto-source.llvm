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

The record is one-shot and requires host-owned, nonzero actor/slot/task/code
generations. Stale foreign producer generations are rejected. No allocations
occur on normal task start/await/resume paths after host initialization.
The scheduler's existing eight WAKE tickets per actor bound outstanding
continuation admission.

## Explicit lifecycle boundary

This bridge is not a complete actor TaskScope. The host must cancel live
suspended Tasks before calling `ores_native_actor_scheduler_stop_sealed`.
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
- actor STOP while suspended, cancelled transferred wake, no guest resurrection
  from late foreign completion, and actor `wait_done` physical fencing.

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
