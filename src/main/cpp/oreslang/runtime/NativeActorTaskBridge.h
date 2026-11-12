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
 * The host MUST use task_cancel() (or task_stop_actor()) before actor STOP if
 * a Task can be suspended on an unfulfilled foreign Future. Otherwise the
 * actor's physical DONE remains fenced by its retained resource loan.
 */
typedef enum {
    ORES_ACTOR_TASK_NEW = 0,
    ORES_ACTOR_TASK_QUEUED = 1,
    ORES_ACTOR_TASK_RUNNING = 2,
    ORES_ACTOR_TASK_SUSPENDED = 3,
    ORES_ACTOR_TASK_CANCEL_QUEUED = 4,
    ORES_ACTOR_TASK_FINISH_PENDING = 5,
    ORES_ACTOR_TASK_RETIRED = 6
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
/* After a claimed guest turn returns AWAIT, reserve and transfer one future
 * WAKE credit without parking its actor carrier. Only a successful suspend
 * permits external-producer completion to publish. */
int ores_native_actor_task_suspend(OresNativeActorTask* task);
/* Foreign Future producer: MAY publish READY/CANCELLED/FAILED, but never runs
 * guest code. The scheduler then runs on_wake on the actor worker pool.
 * task_generation prevents a stale producer from waking a reused record. */
int ores_native_actor_task_publish(
    OresNativeActorTask* task, uint64_t task_generation,
    OresNativeWakeOutcome outcome);
/* Called inside the same authenticated actor wake, after DONE/error.
 * Releases the actor-incarnation physical loan exactly once. */
int ores_native_actor_task_finish(OresNativeActorTask* task);
/* Cancellation is nonparking: suspended transferred wake is aborted, queued
 * wake is drained without guest execution, running guest finishes at its
 * cooperative boundary. STOP may only finish after this receipt retires. */
int ores_native_actor_task_cancel(OresNativeActorTask* task);
int ores_native_actor_task_stop_actor(OresNativeActorTask* task);
int ores_native_actor_task_cancel_requested(OresNativeActorTask* task);
int ores_native_actor_task_physically_done(OresNativeActorTask* task);
/* Requires NEW or physically RETIRED; never destroy a live mutex/record. */
int ores_native_actor_task_close(OresNativeActorTask* task);

#ifdef __cplusplus
}
#endif
