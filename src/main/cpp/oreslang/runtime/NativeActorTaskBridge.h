#pragma once
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Bounded, host-owned continuation record: no thread or pool per Task.
 * The caller allocates one record per logical Task and keeps it alive until
 * ores_native_actor_task_close() succeeds. It may be embedded in an actor heap.
 *
 * This staging API intentionally does not lower guest task syntax, compile
 * capture environments, or implement structured TaskScope/async defer.
 * For source actors, the compiler/host MUST use NativeActorTaskRegistry:
 * registry_start admits every Task and registry_stop atomically seals and
 * revokes all registered descendants before actor STOP. Bare trusted-host
 * tasks outside a registry still require explicit task_cancel() before STOP.
 * An unrevoked suspended Task retains its actor's physical DONE resource
 * loan indefinitely; this is a deliberate fail-closed quarantine, not a
 * reason to manufacture Task completion.
 */
typedef enum {
    ORES_ACTOR_TASK_NEW = 0,
    ORES_ACTOR_TASK_QUEUED = 1,
    ORES_ACTOR_TASK_RUNNING = 2,
    /* Requested await, but the physical guest WAKE callback still owns the lane. */
    ORES_ACTOR_TASK_SUSPENDING = 3,
    ORES_ACTOR_TASK_SUSPENDED = 4,
    ORES_ACTOR_TASK_CANCEL_QUEUED = 5,
    ORES_ACTOR_TASK_FINISH_PENDING = 6,
    ORES_ACTOR_TASK_RETIRED = 7
} OresNativeActorTaskPhase;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeWakeTicket ticket;
    OresNativeWakeTicket active_ticket; /* guest callback pending physical ACK */
    uint64_t task_slot;
    uint64_t task_generation;
    uint64_t code_generation;
    uint64_t next_descriptor_generation;
    OresNativeActorTaskPhase phase;
    int initialized;
    int resource_loan;
    int in_wake; /* remains true until scheduler's AFTER-WAKE hook */
    int cancellation_requested;
    /* At most one Future producer outcome may arrive while SUSPENDING.
     * It is held in this record, never published as a WAKE until the
     * authenticated after-WAKE physical-turn exit acknowledgement. */
    int pending_early_outcome;
    OresNativeWakeOutcome early_outcome;
} OresNativeActorTask;

/* Initialized at host actor creation: task slot/generation/code generation
 * are nonzero, compiler/host-owned, never guest-selected identifiers.
 * Callers provide preallocated memory; no hot-path allocation occurs. */
int ores_native_actor_task_init(
    OresNativeActorTask* task, OresNativeActorScheduler* scheduler,
    OresNativeActorRef actor, uint64_t slot, uint64_t task_generation,
    uint64_t code_generation);
/* Only authenticated actor DATA or READY WAKE may start a Task. Reserves a
 * ticket and submits the initial turn through the SAME actor scheduler.
 * STOP wins admission under the scheduler's lock, not a stale preflight. */
int ores_native_actor_task_start(OresNativeActorTask* task);
/* Called solely by scheduler on_wake. Returns 1 iff the exact matching
 * READY ticket claimed a live actor's worker carrier. A cancelled/stopped
 * wake physically retires its record without executing guest code. */
int ores_native_actor_task_claim_wake(
    OresNativeActorTask* task, OresNativeWakeTicket ticket);
/* While a claimed guest turn is still executing, reserve/transfer one future
 * WAKE credit without parking the actor carrier. This enters SUSPENDING,
 * which is NOT physically suspended: producers cannot publish yet. Only the
 * trusted scheduler after_wake callback may transition to SUSPENDED after
 * the original guest callback exits and its execution claim is released. */
int ores_native_actor_task_suspend(OresNativeActorTask* task);
/* Foreign Future producer: MAY submit one READY/CANCELLED/FAILED outcome,
 * but never runs guest code. An outcome arriving during SUSPENDING is
 * escrowed and published only after the scheduler's post-WAKE physical
 * callback-return acknowledgement; duplicates are rejected. The scheduler
 * then runs on_wake on the owning actor pool. task_generation fences stale
 * producers from waking a reused record. */
int ores_native_actor_task_publish(
    OresNativeActorTask* task, uint64_t task_generation,
    OresNativeWakeOutcome outcome);
/* Called inside the same authenticated actor wake, after DONE/error.
 * Marks guest-complete. Physical loan retirement is deferred until the
 * native scheduler has actually unwound this guest WAKE callback/claim. */
int ores_native_actor_task_finish(OresNativeActorTask* task);
/* Must be wired to OresNativeActorSchedulerOptions.after_wake. Executes with
 * guest context ALREADY CLEARED, no scheduler lock held, and lane physically
 * pinned. The last guest frame has exited before this acknowledgement.
 * Converts SUSPENDING -> SUSPENDED or retires FINISH_PENDING; a canceled or
 * still-running callback never fabricates physical retirement. */
int ores_native_actor_task_after_wake(
    OresNativeActorTask* task, OresNativeWakeTicket ticket);
/* Cancellation is nonparking: suspended transferred wake is aborted, queued
 * wake is drained without guest execution, running guest finishes at its
 * cooperative boundary. STOP may only finish after this receipt retires. */
int ores_native_actor_task_cancel(OresNativeActorTask* task);
int ores_native_actor_task_stop_actor(OresNativeActorTask* task);
int ores_native_actor_task_cancel_requested(OresNativeActorTask* task);
int ores_native_actor_task_physically_done(OresNativeActorTask* task);
/* Internal registry authority: parent must be RUNNING in its own READY WAKE
 * on this exact physical actor carrier before child Task admission. */
int ores_native_actor_task_is_current_running(OresNativeActorTask* task);
/* Requires NEW or physically RETIRED; never destroy a live mutex/record. */
int ores_native_actor_task_close(OresNativeActorTask* task);

#ifdef __cplusplus
}
#endif
