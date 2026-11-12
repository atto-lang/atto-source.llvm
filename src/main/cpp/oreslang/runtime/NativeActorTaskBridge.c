#include "oreslang/runtime/NativeActorTaskBridge.h"
#include <limits.h>
#include <string.h>

/* The bridge holds only its record mutex when calling the native scheduler.
 * Scheduler callbacks always occur after its lock has been released. Never
 * enter guest code while holding the bridge mutex. */
static int same_actor(OresNativeActorRef a, OresNativeActorRef b) {
    return a.scheduler_domain == b.scheduler_domain
        && a.actor_slot == b.actor_slot && a.generation == b.generation;
}
static int same_ticket(OresNativeWakeTicket a, OresNativeWakeTicket b) {
    return same_actor(a.producer, b.producer)
        && same_actor(a.target, b.target)
        && a.slot == b.slot && a.slot_generation == b.slot_generation
        && a.continuation_slot == b.continuation_slot
        && a.continuation_generation == b.continuation_generation
        && a.code_generation == b.code_generation
        && a.descriptor_generation == b.descriptor_generation;
}
static int next_wake(OresNativeActorTask* t, OresNativeWakeTicket* out) {
    if (t->next_descriptor_generation == UINT64_MAX) return 0;
    ++t->next_descriptor_generation;
    if (!ores_native_actor_scheduler_reserve_wake(
            t->scheduler, t->actor, t->actor,
            t->task_slot, t->task_generation, t->code_generation,
            t->next_descriptor_generation, out))
        return 0;
    if (!ores_native_actor_scheduler_transfer_wake(t->scheduler, *out)) {
        /* A STOP winner rejects irreversible transfer. A reserved wake is
         * still cancellable; release its physical loan before failing. */
        if (!ores_native_actor_scheduler_cancel_wake(t->scheduler, *out))
            return 0; /* quarantine: never forge DONE */
        return 0;
    }
    return 1;
}
static int retire_locked(OresNativeActorTask* t) {
    if (!t->resource_loan) return 0;
    /* Do not publish RETIRED unless the scheduler acknowledged the exact
     * actor-incarnation release; a failing release quarantines state. */
    if (!ores_native_actor_scheduler_release_resource(t->scheduler, t->actor))
        return 0;
    t->resource_loan = 0;
    t->phase = ORES_ACTOR_TASK_RETIRED;
    return 1;
}
int ores_native_actor_task_init(
        OresNativeActorTask* t, OresNativeActorScheduler* s,
        OresNativeActorRef actor, uint64_t slot, uint64_t generation,
        uint64_t code_generation) {
    if (!t || !s || !actor.scheduler_domain || !actor.generation
            || !slot || !generation || !code_generation)
        return 0;
    /* The caller owns a fresh or already closed allocation; it must not
     * reinitialize a live mutex. */
    memset(t, 0, sizeof(*t));
    if (pthread_mutex_init(&t->mutex, NULL) != 0) return 0;
    t->scheduler = s;
    t->actor = actor;
    t->task_slot = slot;
    t->task_generation = generation;
    t->code_generation = code_generation;
    t->phase = ORES_ACTOR_TASK_NEW;
    t->initialized = 1;
    return 1;
}
int ores_native_actor_task_start(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    int ok = t->phase == ORES_ACTOR_TASK_NEW
        && ores_native_actor_scheduler_can_start_task(t->scheduler, t->actor)
        && ores_native_actor_scheduler_retain_resource(t->scheduler, t->actor);
    if (!ok) { pthread_mutex_unlock(&t->mutex); return 0; }
    t->resource_loan = 1;
    OresNativeWakeTicket wake = {0};
    if (!next_wake(t, &wake)) {
        (void)retire_locked(t);
        pthread_mutex_unlock(&t->mutex);
        return 0;
    }
    t->ticket = wake;
    t->phase = ORES_ACTOR_TASK_QUEUED;
    OresNativeWakePublishStatus published =
        ores_native_actor_scheduler_publish_wake(
            t->scheduler, wake, ORES_NATIVE_WAKE_READY);
    if (published != ORES_NATIVE_WAKE_PUBLISHED) {
        if (published == ORES_NATIVE_WAKE_PRODUCER_RETIRED
                || ores_native_actor_scheduler_abort_transferred_wake(
                    t->scheduler, wake))
            (void)retire_locked(t);
        ok = 0;
    }
    pthread_mutex_unlock(&t->mutex);
    return ok;
}
int ores_native_actor_task_claim_wake(
        OresNativeActorTask* t, OresNativeWakeTicket wake) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    if ((t->phase != ORES_ACTOR_TASK_QUEUED
            && t->phase != ORES_ACTOR_TASK_CANCEL_QUEUED)
            || !same_ticket(wake, t->ticket)
            || !ores_native_actor_scheduler_is_current_wake(
                t->scheduler, wake)) {
        pthread_mutex_unlock(&t->mutex);
        return 0;
    }
    if (t->cancellation_requested
            || !ores_native_actor_scheduler_is_current_ready_wake(
                t->scheduler, wake)) {
        (void)retire_locked(t);
        pthread_mutex_unlock(&t->mutex);
        return 0;
    }
    t->phase = ORES_ACTOR_TASK_RUNNING;
    pthread_mutex_unlock(&t->mutex);
    return 1;
}
int ores_native_actor_task_suspend(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    int ok = t->phase == ORES_ACTOR_TASK_RUNNING
        && !t->cancellation_requested
        && ores_native_actor_scheduler_can_start_task(t->scheduler, t->actor)
        && ores_native_actor_scheduler_is_current_wake(t->scheduler, t->ticket);
    if (ok) {
        OresNativeWakeTicket wake = {0};
        ok = next_wake(t, &wake);
        if (ok) {
            t->ticket = wake;
            t->phase = ORES_ACTOR_TASK_SUSPENDED;
        }
    }
    pthread_mutex_unlock(&t->mutex);
    return ok;
}
int ores_native_actor_task_publish(
        OresNativeActorTask* t, uint64_t generation,
        OresNativeWakeOutcome outcome) {
    if (!t || !t->initialized || (outcome != ORES_NATIVE_WAKE_READY
            && outcome != ORES_NATIVE_WAKE_CANCELLED
            && outcome != ORES_NATIVE_WAKE_FAILED))
        return 0;
    pthread_mutex_lock(&t->mutex);
    int ok = generation == t->task_generation
        && t->phase == ORES_ACTOR_TASK_SUSPENDED
        && !t->cancellation_requested;
    if (ok) {
        /* The producer commits only a scheduler wake. It has no guest carrier
         * claim and cannot perform Task.resume or touch sealed actor state. */
        OresNativeWakePublishStatus published =
            ores_native_actor_scheduler_publish_wake(
                t->scheduler, t->ticket, outcome);
        if (published == ORES_NATIVE_WAKE_PUBLISHED) {
            t->phase = ORES_ACTOR_TASK_QUEUED;
        } else {
            if (published == ORES_NATIVE_WAKE_PRODUCER_RETIRED
                    || ores_native_actor_scheduler_abort_transferred_wake(
                        t->scheduler, t->ticket))
                (void)retire_locked(t);
            ok = 0;
        }
    }
    pthread_mutex_unlock(&t->mutex);
    return ok;
}
int ores_native_actor_task_finish(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    int ok = (t->phase == ORES_ACTOR_TASK_RUNNING)
        && ores_native_actor_scheduler_is_current_wake(t->scheduler, t->ticket)
        && retire_locked(t);
    pthread_mutex_unlock(&t->mutex);
    return ok;
}
int ores_native_actor_task_cancel(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    int ok = t->phase != ORES_ACTOR_TASK_RETIRED
        && t->phase != ORES_ACTOR_TASK_NEW && !t->cancellation_requested;
    if (ok) {
        t->cancellation_requested = 1;
        if (t->phase == ORES_ACTOR_TASK_SUSPENDED) {
            if (ores_native_actor_scheduler_abort_transferred_wake(
                    t->scheduler, t->ticket))
                (void)retire_locked(t);
            /* Failed abort means a scheduler or publication contract broke:
             * preserve the loan and quarantine, never manufacture DONE. */
        } else if (t->phase == ORES_ACTOR_TASK_QUEUED) {
            t->phase = ORES_ACTOR_TASK_CANCEL_QUEUED;
        } /* RUNNING is retired only after its genuine guest turn unwinds. */
    }
    pthread_mutex_unlock(&t->mutex);
    return ok;
}
int ores_native_actor_task_stop_actor(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    /* Actor resource loans keep physical DONE false until a cancelled queued
     * wake is drained or a running turn exits. No blocking actor carrier. */
    (void)ores_native_actor_task_cancel(t);
    return ores_native_actor_scheduler_stop_sealed(t->scheduler, t->actor);
}
int ores_native_actor_task_cancel_requested(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    int flag = t->cancellation_requested;
    pthread_mutex_unlock(&t->mutex);
    return flag;
}
int ores_native_actor_task_physically_done(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    int done = t->phase == ORES_ACTOR_TASK_RETIRED && !t->resource_loan;
    pthread_mutex_unlock(&t->mutex);
    return done;
}
int ores_native_actor_task_close(OresNativeActorTask* t) {
    if (!t || !t->initialized) return 0;
    pthread_mutex_lock(&t->mutex);
    int ok = t->phase == ORES_ACTOR_TASK_NEW
        || (t->phase == ORES_ACTOR_TASK_RETIRED && !t->resource_loan);
    if (ok) t->initialized = 0;
    pthread_mutex_unlock(&t->mutex);
    if (ok && pthread_mutex_destroy(&t->mutex) != 0) return 0;
    return ok;
}
