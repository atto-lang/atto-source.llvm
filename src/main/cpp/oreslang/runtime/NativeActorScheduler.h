#pragma once
#include <stddef.h>
#include <stdint.h>
#include "oreslang/runtime/NativeExecutionContext.h"
typedef struct OresNativePhysicalTurnGate OresNativePhysicalTurnGate;
#ifdef __cplusplus
extern "C" {
#endif
typedef struct OresNativeActorScheduler OresNativeActorScheduler;
/* uint32_t intrusive queue indices support bounded million-slot schedulers. */
enum { ORES_NATIVE_SCHEDULER_MAX_ACTORS = 1 << 20 };
typedef struct {
    uint64_t scheduler_domain;
    uint64_t actor_slot;
    uint64_t generation;
} OresNativeActorRef;

/* Native scheduler-owned continuation wake escrow. Each producer actor has a
 * finite, preallocated pool, separate from DATA and supervisor CONTROL. This
 * is a trusted host ABI: source guest programs cannot mint wake tickets.
 * Source-level send_async lowering and physical message loans are NOT supplied
 * by these scheduling primitives alone.
 */
enum { ORES_NATIVE_WAKE_CREDITS_PER_ACTOR = 8 };
/* Runtime-owned actor Task turns are queued separately from DATA/CONTROL.
 * Fixed per-actor capacity, allocated only when on_task is configured.
 * This host ABI does not itself enable source-language TaskExpr lowering. */
enum { ORES_NATIVE_TASK_SLOTS_PER_ACTOR = 8 };
typedef enum {
    ORES_NATIVE_WAKE_READY = 1,
    ORES_NATIVE_WAKE_CANCELLED = 2,
    ORES_NATIVE_WAKE_FAILED = 3
} OresNativeWakeOutcome;
typedef struct {
    OresNativeActorRef producer;
    OresNativeActorRef target;
    uint64_t slot; /* per-producer preallocated wake slot, zero-based */
    uint64_t slot_generation;
    uint64_t continuation_slot;
    uint64_t continuation_generation;
    uint64_t code_generation;
    uint64_t descriptor_generation;
} OresNativeWakeTicket;
typedef enum {
    ORES_NATIVE_WAKE_INVALID = 0,
    ORES_NATIVE_WAKE_PUBLISHED = 1,
    ORES_NATIVE_WAKE_PRODUCER_RETIRED = 2
} OresNativeWakePublishStatus;

typedef enum {
    ORES_NATIVE_TERMINATION_STOPPED = 1,
    ORES_NATIVE_TERMINATION_RUNTIME_RETIRED = 2
} OresNativeActorTerminationKind;

typedef struct {
    OresNativeActorRef actor;
    OresNativeActorTerminationKind kind;
    uint64_t accepted_user_messages;
    uint64_t processed_user_messages;
    uint64_t abandoned_user_messages;
    uint64_t abandoned_control_messages;
} OresNativeActorTerminalState;
enum { ORES_NATIVE_CONTROL_PANIC = 1, ORES_NATIVE_CONTROL_CANCELLED = 2,
       ORES_NATIVE_CONTROL_STOPPED = 3 };
typedef struct {
    uint64_t kind;
    OresNativeActorRef child;
    uint64_t failure_code;
    uint64_t correlation;
} OresNativeActorTerminalFailure;
/* Typed terminal failure is copied into the reserved parent CONTROL slot.
 * A supervisor handler runs only on the parent's single logical actor lane.
 */
typedef void (*OresNativeActorPanicFn)(
    void* ctx, size_t parent_actor, OresNativeActorTerminalFailure failure);
/* Explicit *native host* actor lifecycle, not compiler-lowered source spawn.
 * Spawning activates exactly one preallocated actor slot; no ambient cross-
 * actor mutable references, host heap allocation, or slot reincarnation.
 */
/* Trusted host-only pre-admission wiring. Must be called before the first
 * actor message/carrier starts. Gate outlives this scheduler and is released
 * after its last callback finishes. A NULL gate leaves legacy dispatch intact. */
int ores_native_actor_scheduler_configure_turn_gate(
    OresNativeActorScheduler* scheduler, OresNativePhysicalTurnGate* gate);
/* Wake idle carriers that lost a cross-domain permit race, including while
 * the scheduler is retiring and still has admitted work to drain. Trusted
 * VM-only call; not a guest CONTROL message. */
void ores_native_actor_scheduler_signal_turn_gate(OresNativeActorScheduler* scheduler);
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* scheduler, size_t actor_slot);
/* Send succeeds only after atomic admission to a live, generation-matched
 * actor mailbox. It does NOT promise execution or successful processing.
 * No guest callback, blocking wait, or memory allocation occurs here.
 * Cross-process admission cannot be inferred from an IPC write: callers must
 * obtain a remote mailbox receipt before reporting ACCEPTED.
 */
typedef enum {
    ORES_NATIVE_SEND_ACCEPTED = 0,
    ORES_NATIVE_SEND_INVALID_REF = 1,
    ORES_NATIVE_SEND_TERMINATED = 2,
    ORES_NATIVE_SEND_MAILBOX_FULL = 3,
    ORES_NATIVE_SEND_RESOURCE_EXHAUSTED = 4,
    ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE = 5
} OresNativeSendStatus;
OresNativeSendStatus ores_native_actor_scheduler_try_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
/* Trusted runtime continuation admission from a physical actor DATA, WAKE
 * or TASK turn only (never CONTROL, root threads, foreign actors or producers).
 * Payload is an opaque runtime-owned continuation handle, not guest data.
 * Returns immediately; no inline execution, new pool, or blocking await.
 * STOP rejects new admission and routes previously queued continuations to
 * on_task_cancel instead of on_task. The callback owns handle reclamation.
 * There is deliberately no external-producer entry point: producer events
 * must first enter through an authenticated native WAKE ticket.
 * The compiler does not yet lower TaskExpr to this runtime API. */
typedef enum {
    ORES_NATIVE_TASK_ACCEPTED = 0,
    ORES_NATIVE_TASK_INVALID_ACTOR = 1,
    ORES_NATIVE_TASK_FOREIGN_CARRIER = 2,
    ORES_NATIVE_TASK_STOPPED = 3,
    ORES_NATIVE_TASK_MAILBOX_FULL = 4,
    ORES_NATIVE_TASK_RESOURCE_EXHAUSTED = 5,
    ORES_NATIVE_TASK_NOT_CONFIGURED = 6
} OresNativeTaskAdmissionStatus;
OresNativeTaskAdmissionStatus ores_native_actor_scheduler_try_schedule_task(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    uint64_t task_handle);
/* Trusted host-only admission receipt. On ACCEPTED, returns the exact native
 * DATA sequence minted under the scheduler lock for this target incarnation.
 * On denial, out_sequence is set to 0; NULL is invalid. This is not a guest
 * serializable capability or a handler-completion acknowledgement.
 * The source-target typed transport binds this to its opaque descriptor to
 * prevent cross-mail ticket substitution or replay. */
OresNativeSendStatus ores_native_actor_scheduler_try_send_receipted(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    uint64_t payload, uint64_t* out_sequence);
/* Compatibility API: 1 iff accepted, 0 otherwise. Prefer typed try_send. */
int ores_native_actor_scheduler_send(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor, uint64_t payload);
int ores_native_actor_scheduler_stop(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Seal user DATA immediately while preserving an already-active turn and
 * lifecycle CONTROL. Queued DATA is abandoned with terminal accounting. */
int ores_native_actor_scheduler_stop_sealed(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/*
 * Trusted runtime external-resource loan. Retain linearizes against STOP under
 * the scheduler lock; a successful retain delays terminal publication and
 * get_done until the exact incarnation releases it. Release remains valid
 * after STOP but stale/recycled ActorRefs fail closed.
 */
int ores_native_actor_scheduler_retain_resource(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_release_resource(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Nonblocking host readiness/termination observations. Source-level
 * get_ready()/get_done() must return futures when compiler lowering exists.
 */
int ores_native_actor_scheduler_poll_ready(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_poll_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
int ores_native_actor_scheduler_terminal_state(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor,
    OresNativeActorTerminalState* output);
/* Trusted external supervisor barrier: block only the host caller until
 * the selected stopped actor drains. This is NOT a guest await/Future;
 * never invoke it on a scheduler worker/actor carrier. */
int ores_native_actor_scheduler_wait_done(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Handler runs on the scheduler worker pool, never on the submitting carrier.
 * At most one callback executes per actor, across all worker threads.
 * The actor index is a scheduler-local slot, not a cross-tenant capability.
 */
typedef void (*OresNativeActorTaskFn)(void* ctx, size_t actor_index, uint64_t payload);
typedef void (*OresNativeActorWakeFn)(
    void* ctx, size_t actor_index, OresNativeWakeTicket ticket,
    OresNativeWakeOutcome outcome);
/* One reserved CONTROL message per actor, independent of normal-mail
 * capacity; only an admitted actor lane may invoke the CONTROL handler.
 * CONTROL callbacks never run on a failed child's carrier by direct call.
 */
/* Variant with a dedicated continuation wake callback. The callback is
 * dispatched under the producer's authenticated single-flight actor claim,
 * never inline from completion, admission or cancellation threads. */
OresNativeActorScheduler* ores_native_actor_scheduler_open_with_wakes(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_control,
    OresNativeActorPanicFn on_panic, OresNativeActorWakeFn on_wake, void* ctx);
/* Scheduler-lock protocol, fixed at open.
 * PRIORITY_INHERIT (default for every legacy constructor): bounded priority
 *   inversion, required by hard-RT admission. On Linux every *contended*
 *   acquire is kernel-arbitrated (FUTEX_LOCK_PI, no user-space spinning), so
 *   heavy producer/carrier contention costs ~25x in throughput.
 * THROUGHPUT: plain (glibc: adaptive-spin) mutex. No priority inheritance:
 *   priority_inheritance_enabled() reports 0, so hard-RT admission that
 *   requires it fails closed. Use for general-purpose domains only. */
typedef enum {
    ORES_NATIVE_SCHEDULER_LOCK_PRIORITY_INHERIT = 0,
    ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT = 1
} OresNativeSchedulerLockProtocol;
typedef struct {
    size_t actor_count;
    size_t per_actor_capacity;
    size_t workers;
    OresNativeActorTaskFn on_message;  /* required */
    OresNativeActorTaskFn on_control;  /* optional */
    OresNativeActorPanicFn on_panic;   /* optional */
    OresNativeActorWakeFn on_wake;     /* optional; enables wake escrow */
    void* ctx;
    OresNativeSchedulerLockProtocol lock_protocol;
    /* Optional paired callbacks. on_task runs only for an admitted live
     * actor Task; on_task_cancel handles an admitted Task after STOP.
     * Both run under the exact actor's single-writer physical carrier claim.
     * Pairing is mandatory: silently dropping cleanup after STOP is unsafe. */
    OresNativeActorTaskFn on_task;
    OresNativeActorTaskFn on_task_cancel;
} OresNativeActorSchedulerOptions;
/* Superset of every open_* variant. Returns NULL for invalid options,
 * including an unknown lock protocol. */
OresNativeActorScheduler* ores_native_actor_scheduler_open_with_options(
    const OresNativeActorSchedulerOptions* options);
OresNativeActorScheduler* ores_native_actor_scheduler_open_typed_control(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_future,
    OresNativeActorPanicFn on_panic, void* ctx);
OresNativeActorScheduler* ores_native_actor_scheduler_open_control(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn on_message, OresNativeActorTaskFn on_control, void* ctx);
/* Trusted host-only immutable child-parent reservation: before the first
 * child DATA message, bind one live incarnation to its supervisor. The
 * scheduler preallocates one terminal receipt per actor at creation.
 */
int ores_native_actor_scheduler_bind_supervisor(
    OresNativeActorScheduler* scheduler,
    OresNativeActorRef parent, OresNativeActorRef child);
/* A distinct bound child publishes one typed terminal failure into its
 * preallocated receipt; parent CONTROL owns the eventual single-flight
 * delivery/ack even under a saturated USER mailbox. No manual retry is
 * required for an already-admitted terminal receipt. Duplicates, stale,
 * foreign, unbound and wrong-parent refs are rejected. Trusted host ABI.
 */
/* Reserve the exact future delivery credit BEFORE registering a bounded
 * pending mailbox producer. All worker pthreads are made serviceable during
 * reservation, so transfer->publish cannot fail on late worker creation.
 * target/continuation/descriptor identities are immutable ABA fences.
 * A reserved or transferred ticket blocks producer physical DONE; it does not
 * occupy the supervisor CONTROL slot or the USER mailbox.
 *
 * The mailbox-accounting lock must serialize TRANSFER with mailbox credit
 * handoff and cancellation. For same-scheduler targets, TRANSFER revalidates
 * the exact live target incarnation so STOP cannot become a false admission.
 * Wake mutation APIs fail closed when invoked on a scheduler created without
 * wake support. No guest/user callback runs in these functions.
 */
int ores_native_actor_scheduler_reserve_wake(
    OresNativeActorScheduler* scheduler, OresNativeActorRef producer,
    OresNativeActorRef target, uint64_t continuation_slot,
    uint64_t continuation_generation, uint64_t code_generation,
    uint64_t descriptor_generation, OresNativeWakeTicket* out);
/* Verifies a ticket is currently RESERVED in this scheduler's own wake
 * credit table and was reserved during the current authenticated DATA-mail
 * turn with this original mail ordinal. False for copied/old tickets,
 * foreign producers, generic callbacks, other mails or STOP. This is
 * trusted-host capture-only; it does not grant authority at resume. */
int ores_native_actor_scheduler_current_data_issued_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket,
    uint64_t expected_original_mail);
int ores_native_actor_scheduler_transfer_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
/* Cancellation succeeds only while still RESERVED (not transferred). */
int ores_native_actor_scheduler_cancel_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
/* Explicit rollback for an aborted transfer before envelope publication.
 * This is NOT late producer cancellation and must settle the send as failed. */
int ores_native_actor_scheduler_abort_transferred_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
/* After transfer, publication cannot fail due to capacity or allocation.
 * If STOP won first it consumes the reserved ticket as a terminal failure,
 * without scheduling user code or fabricating successful delivery. */
OresNativeWakePublishStatus ores_native_actor_scheduler_publish_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket,
    OresNativeWakeOutcome outcome);
int ores_native_actor_scheduler_is_current_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
/* Exact scheduler-issued WAKE ticket, READY outcome, current physical
 * producer claim and RUNNING phase are all required. CANCELLED/FAILED wakes
 * and STOPPING cleanup return 0. Does not confer request identity by itself:
 * trusted source admission must snapshot the originating DATA-mail receipt
 * into an owned continuation before suspension. */
int ores_native_actor_scheduler_is_current_ready_wake(
    OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket);
size_t ores_native_actor_scheduler_outstanding_wake_loans(
    OresNativeActorScheduler* scheduler, OresNativeActorRef producer);
uint64_t ores_native_actor_scheduler_wakes_completed(
    OresNativeActorScheduler* scheduler);
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent, uint64_t payload);
/* Child must be a valid actor in the same scheduler, distinct from parent.
 * Returns 0 on reserved CONTROL saturation, stale refs or parent retirement.
 * Never invokes on_panic directly on the publishing child's carrier.
 */
int ores_native_actor_scheduler_publish_failure(
    OresNativeActorScheduler* scheduler, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure);
uint64_t ores_native_actor_scheduler_control_completed(
    OresNativeActorScheduler* scheduler);
OresNativeActorScheduler* ores_native_actor_scheduler_open(
    size_t actor_count, size_t per_actor_capacity, size_t workers,
    OresNativeActorTaskFn callback, void* ctx);
/* Legacy index admission requires an already-spawned host actor.\n * New hosts should use the generation-checked actor ref send API.\n * O(1), nonblocking admission. Returns 1 on accepted, 0 on capacity or retirement.
 * Does not allocate and never invokes callbacks inline. Capacity includes the
 * currently executing task. Caller retains ownership on rejection.
 */
int ores_native_actor_scheduler_submit(
    OresNativeActorScheduler* scheduler, size_t actor_index, uint64_t payload);
/* No new tasks after retirement, but previously accepted tasks drain. */
void ores_native_actor_scheduler_retire(OresNativeActorScheduler* scheduler);
/* Wait for every accepted callback to complete; may not be called from its worker. */
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* scheduler);
/* Whether this OS thread is currently executing a callback from scheduler.
 * Only intended for immediate deadlock prevention in VM teardown. */
int ores_native_actor_scheduler_is_current_carrier(OresNativeActorScheduler* scheduler);
/*
 * Trusted-host bounded, nonallocating commit. Both exact actor generations
 * must be READY/RUNNING/SUSPENDED and not stopping or retired, serialized
 * under the SAME scheduler lifecycle mutex used by STOP/retirement.
 * The callback MUST NOT allocate, suspend, lock other mutexes, call a
 * scheduler API, run guest code or destructors, or block the carrier.
 * This is NOT a guest ABI, transport for functions, or a generic executor.
 */
typedef int (*OresNativeActorBoundedCommitV1)(void* opaque);
int ores_native_actor_scheduler_commit_if_both_live_v1(
    OresNativeActorScheduler* scheduler, OresNativeActorRef sender,
    OresNativeActorRef recipient, OresNativeActorBoundedCommitV1 commit,
    void* opaque);
/* Trusted host-only guard: true during ANY native actor scheduler callback,
 * regardless of which scheduler owns the carrier. This does not grant actor
 * authority; callers use it to reject unsafe fallback to foreign/root pools.
 * Returns zero on external producer threads and between actor turns. */
int ores_native_actor_scheduler_in_any_actor_turn(void);
/* Exact live actor identity for trusted callback/FFI bridges only. */
int ores_native_actor_scheduler_is_current_actor(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor);
/* Original DATA-mail admission sequence for the current authenticated
 * physical actor carrier only. Returns 0 (without modifying *out_sequence)
 * on any non-DATA, STOP, foreign actor/thread or nonexecuting context.
 * The sequence is minted and stored alongside the DATA mailbox payload at
 * successful admission; WAKE/SourceTask resume requires a separate issuance
 * path and is intentionally unbound here. This is a trusted-host API, not
 * a guest-callable source of HTTP request authority. */
int ores_native_actor_scheduler_current_data_mail_sequence(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    uint64_t* out_sequence);
/* Logical execution identity for the current guest turn. The execution id is
 * not a heap id and is not a carrier/thread id. For actor turns the allocation
 * domain names the actor-owned domain independently. Returns zero outside an
 * authenticated current turn. */
int ores_native_actor_scheduler_current_execution_context(
    OresNativeActorScheduler* scheduler, OresNativeExecutionContext* out);
/* Host CONTROL-plane wake only: signals an existing scheduler condition.
 * Does not admit actor mail, start workers, invoke guest code, or count as
 * a supervisor delivery receipt. Returns zero once retired.
 * Caller must fence the scheduler's lifetime. */
int ores_native_actor_scheduler_signal_control(OresNativeActorScheduler* scheduler);
/* Requires retirement, drained tasks AND caller-owned submitter fence.
 * Returns 0 when not ready; after success the pointer is invalid.
 */
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* scheduler);
/* Configure all subsequently created carriers for explicit POSIX SCHED_FIFO.
 * Bootstrap-only: must precede carrier creation and message/control admission.
 * Configuration success does not imply the process has permission to create
 * such threads; prestart fails closed if pthread_create rejects the policy. */
int ores_native_actor_scheduler_configure_fifo(
    OresNativeActorScheduler* scheduler, int priority);
/* Bounded idle spin: before parking in the kernel, an idle carrier busy-polls
 * for new work for at most spin_ns (0 disables; max 1,000,000). A handoff that
 * arrives inside the budget costs a cache-line transfer instead of a futex
 * sleep + wake, at the price of burning that CPU while idle. Bootstrap-only,
 * same rule as configure_fifo. Spinning never delays CONTROL or retirement:
 * both bump the same work epoch the spinner polls. */
int ores_native_actor_scheduler_configure_idle_spin(
    OresNativeActorScheduler* scheduler, uint32_t spin_ns);
/* Pin carrier k to cpus[k % count] (one CPU per carrier) from thread creation.
 * Linux only; returns 0 on other OSes, for CPUs outside the process's current
 * affinity mask, or after bootstrap. Configuration is not evidence: use
 * cpu_affinity_observed after prestart_carriers. */
int ores_native_actor_scheduler_configure_cpu_affinity(
    OresNativeActorScheduler* scheduler, const uint32_t* cpus, size_t count);
/* True only when affinity was configured, every carrier has started, and each
 * carrier read back an OS affinity of exactly its one configured CPU and was
 * running on it. Required by hard-RT admission; not sufficient for it (IRQ
 * routing, nohz_full, cpuset partitions and memory residency are separate). */
int ores_native_actor_scheduler_cpu_affinity_observed(
    OresNativeActorScheduler* scheduler);
/* True only after every configured SCHED_FIFO carrier has been created. */
int ores_native_actor_scheduler_fifo_carriers_ready(
    OresNativeActorScheduler* scheduler);
/* Move every pthread_create out of the future admission path. This is a
 * necessary but deliberately insufficient hard-RT primitive: it does not
 * qualify scheduler policy, priority inheritance, CPU/IRQ isolation, memory
 * residency, or WCET. Must run before any message/control admission. */
int ores_native_actor_scheduler_prestart_carriers(
    OresNativeActorScheduler* scheduler);
/* Compatibility spelling for RT VM bootstrap; delegates to the stronger
 * current prestart-carriers invariant rather than maintaining another state. */
int ores_native_actor_scheduler_prestart_realtime(
    OresNativeActorScheduler* scheduler);
int ores_native_actor_scheduler_all_carriers_started(
    OresNativeActorScheduler* scheduler);
/* True only when the scheduler mutex was successfully initialized with the
 * POSIX priority-inheritance protocol. Hard-RT admission should require this
 * (or a separately qualified synchronization protocol). */
int ores_native_actor_scheduler_priority_inheritance_enabled(
    OresNativeActorScheduler* scheduler);
/* Carriers created on demand unless explicitly prestarted; includes workers
 * until retirement. */
size_t ores_native_actor_scheduler_started_carriers(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_accepted(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_rejected(OresNativeActorScheduler* scheduler);
uint64_t ores_native_actor_scheduler_completed(OresNativeActorScheduler* scheduler);
#ifdef ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE
/* Test-only deterministic worker-create failure; no production symbol. */
void ores_native_actor_scheduler_test_fail_next_create(void);
#endif
#ifdef ORESLANG_NATIVE_TEST_EXECUTION_EXHAUSTION
/* Test-only execution-id exhaustion control; no production symbol. */
int ores_native_actor_scheduler_test_set_next_execution(
    OresNativeActorScheduler* scheduler, uint64_t next_execution);
#endif
#ifdef __cplusplus
}
#endif
