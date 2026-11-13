#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorTaskBridge.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "ACTOR_TASK_WAKE_FAIL %s:%d: %s\\n", __FILE__, __LINE__, #c); \
    abort(); \
} } while (0)

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeActorTask parent, child, control_probe;
    OresNativeWakeTicket finishing_parent_wake;
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    int mode; /* 1=nest+external completion, 2=STOP while suspended */
    int suspended, suspension_physically_returned, allow_suspend_guest_exit;
    int child_done, parent_done, allow_parent_wake_exit;
    atomic_int parent_turns, child_turns, control_turns, errors;
} Fixture;

static void signal_host(Fixture* f, int* field) {
    CHECK(pthread_mutex_lock(&f->mutex) == 0);
    *field = 1;
    CHECK(pthread_cond_broadcast(&f->cv) == 0);
    CHECK(pthread_mutex_unlock(&f->mutex) == 0);
}
static void wait_host(Fixture* f, int* field) {
    CHECK(pthread_mutex_lock(&f->mutex) == 0);
    while (!*field)
        CHECK(pthread_cond_wait(&f->cv, &f->mutex) == 0);
    CHECK(pthread_mutex_unlock(&f->mutex) == 0);
}
static void on_data(void* opaque, size_t actor, uint64_t message) {
    Fixture* f = (Fixture*)opaque;
    if (actor != 0 || message != 1
            || !ores_native_actor_scheduler_is_current_actor(f->scheduler, f->actor)
            || !ores_native_actor_scheduler_can_start_task(f->scheduler, f->actor)
            || !ores_native_actor_task_start(&f->parent))
        atomic_fetch_add(&f->errors, 1);
}
static void on_control(void* opaque, size_t actor, uint64_t message) {
    Fixture* f = (Fixture*)opaque;
    if (actor != 0 || message != 99
            || ores_native_actor_scheduler_can_start_task(f->scheduler, f->actor)
            || ores_native_actor_task_start(&f->control_probe))
        atomic_fetch_add(&f->errors, 1);
    atomic_fetch_add(&f->control_turns, 1);
}
static void on_wake(void* opaque, size_t actor,
        OresNativeWakeTicket wake, OresNativeWakeOutcome outcome) {
    Fixture* f = (Fixture*)opaque;
    if (actor != 0 || outcome != ORES_NATIVE_WAKE_READY
            || !ores_native_actor_scheduler_is_current_wake(f->scheduler, wake)) {
        atomic_fetch_add(&f->errors, 1);
        return;
    }
    if (wake.continuation_slot == 1) {
        if (!ores_native_actor_task_claim_wake(&f->parent, wake))
            return; /* STOP/cancellation does not run guest code */
        int turn = atomic_fetch_add(&f->parent_turns, 1);
        if (turn == 0) {
            if (f->mode == 1 && !ores_native_actor_task_start(&f->child))
                atomic_fetch_add(&f->errors, 1);
            if (!ores_native_actor_task_suspend(&f->parent))
                atomic_fetch_add(&f->errors, 1);
            signal_host(f, &f->suspended);
            /* Test-only barrier: prove a Future producer cannot resume while
             * the original guest callback still owns the actor lease. */
            CHECK(pthread_mutex_lock(&f->mutex) == 0);
            while (!f->allow_suspend_guest_exit)
                CHECK(pthread_cond_wait(&f->cv, &f->mutex) == 0);
            CHECK(pthread_mutex_unlock(&f->mutex) == 0);
        } else if (turn == 1) {
            if (!ores_native_actor_task_finish(&f->parent))
                atomic_fetch_add(&f->errors, 1);
            CHECK(pthread_mutex_lock(&f->mutex) == 0);
            f->finishing_parent_wake = wake;
            CHECK(pthread_mutex_unlock(&f->mutex) == 0);
            signal_host(f, &f->parent_done);
            /* Host-only deterministic barrier: prove task physical DONE is
             * NOT visible while the final guest WAKE is still on-stack.
             * This is not a production guest blocking pattern. */
            CHECK(pthread_mutex_lock(&f->mutex) == 0);
            while (!f->allow_parent_wake_exit)
                CHECK(pthread_cond_wait(&f->cv, &f->mutex) == 0);
            CHECK(pthread_mutex_unlock(&f->mutex) == 0);
        } else atomic_fetch_add(&f->errors, 1);
    } else if (wake.continuation_slot == 2) {
        if (!ores_native_actor_task_claim_wake(&f->child, wake)) {
            atomic_fetch_add(&f->errors, 1);
            return;
        }
        if (!ores_native_actor_scheduler_is_current_actor(f->scheduler, f->actor)
                || !ores_native_actor_task_finish(&f->child))
            atomic_fetch_add(&f->errors, 1);
        atomic_fetch_add(&f->child_turns, 1);
        signal_host(f, &f->child_done);
    } else atomic_fetch_add(&f->errors, 1);
}
static void after_wake(void* opaque, size_t actor,
        OresNativeWakeTicket wake) {
    Fixture* f = (Fixture*)opaque;
    if (actor != 0
            || ores_native_actor_scheduler_is_current_carrier(f->scheduler)
            || !ores_native_actor_scheduler_is_after_wake(f->scheduler, wake)) {
        atomic_fetch_add(&f->errors, 1);
        return;
    }
    OresNativeActorTask* record = wake.continuation_slot == 1
        ? &f->parent : wake.continuation_slot == 2 ? &f->child : NULL;
    if (!record || !ores_native_actor_task_after_wake(record, wake)) {
        atomic_fetch_add(&f->errors, 1);
    } else if (wake.continuation_slot == 1
            && atomic_load(&f->parent_turns) == 1) {
        /* A true scheduler-issued physical-turn return, not a user-visible
         * logical await signal, makes the next WAKE publishable. */
        signal_host(f, &f->suspension_physically_returned);
    }
}
static void open_fixture(Fixture* f, int mode) {
    f->mode = mode;
    CHECK(pthread_mutex_init(&f->mutex, NULL) == 0);
    CHECK(pthread_cond_init(&f->cv, NULL) == 0);
    OresNativeActorSchedulerOptions opts = {
        .actor_count = 1, .per_actor_capacity = 4, .workers = 2,
        .on_message = on_data, .on_control = on_control,
        .on_panic = NULL, .on_wake = on_wake, .ctx = f,
        .lock_protocol = ORES_NATIVE_SCHEDULER_LOCK_PRIORITY_INHERIT,
        .after_wake = after_wake
    };
    f->scheduler = ores_native_actor_scheduler_open_with_options(&opts);
    CHECK(f->scheduler != NULL);
    f->actor = ores_native_actor_scheduler_spawn(f->scheduler, 0);
    CHECK(f->actor.scheduler_domain && f->actor.generation);
    CHECK(ores_native_actor_task_init(
        &f->parent, f->scheduler, f->actor, 1, 1001, 77));
    CHECK(ores_native_actor_task_init(
        &f->child, f->scheduler, f->actor, 2, 1002, 77));
    CHECK(ores_native_actor_task_init(
        &f->control_probe, f->scheduler, f->actor, 3, 1003, 77));
    /* A producer without an authenticated actor lease cannot start a Task. */
    CHECK(!ores_native_actor_task_start(&f->parent));
}
static void close_fixture(Fixture* f) {
    CHECK(!atomic_load(&f->errors));
    CHECK(ores_native_actor_task_close(&f->control_probe));
    CHECK(ores_native_actor_task_close(&f->child));
    CHECK(ores_native_actor_task_close(&f->parent));
    ores_native_actor_scheduler_retire(f->scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(f->scheduler));
    CHECK(pthread_cond_destroy(&f->cv) == 0);
    CHECK(pthread_mutex_destroy(&f->mutex) == 0);
}
int main(void) {
    Fixture f = {0};
    open_fixture(&f, 1);
    CHECK(ores_native_actor_scheduler_send(f.scheduler, f.actor, 1));
    wait_host(&f, &f.suspended);
    CHECK(!ores_native_actor_scheduler_is_current_carrier(f.scheduler));
    CHECK(!ores_native_actor_task_physically_done(&f.parent));
    CHECK(!ores_native_actor_task_publish(&f.parent, 9999, ORES_NATIVE_WAKE_READY));
    /* Even a valid producer generation must fail before after-WAKE physical
     * retirement. Do not confuse logical suspend with callback exit. */
    CHECK(!ores_native_actor_task_publish(&f.parent, 1001, ORES_NATIVE_WAKE_READY));
    signal_host(&f, &f.allow_suspend_guest_exit);
    wait_host(&f, &f.suspension_physically_returned);
    wait_host(&f, &f.child_done);
    CHECK(!ores_native_actor_scheduler_is_current_carrier(f.scheduler));
    CHECK(ores_native_actor_task_publish(
        &f.parent, 1001, ORES_NATIVE_WAKE_READY));
    CHECK(!ores_native_actor_scheduler_is_current_carrier(f->scheduler));
    wait_host(&f, &f.parent_done);
    OresNativeWakeTicket forged_completion;
    CHECK(pthread_mutex_lock(&f.mutex) == 0);
    forged_completion = f.finishing_parent_wake;
    CHECK(pthread_mutex_unlock(&f.mutex) == 0);
    /* Holding the genuine issued ticket must NOT let a host/Future thread
     * forge after-WAKE physical retirement while guest is still on-stack. */
    CHECK(!ores_native_actor_scheduler_is_after_wake(
        f.scheduler, forged_completion));
    CHECK(!ores_native_actor_task_after_wake(
        &f.parent, forged_completion));
    CHECK(!ores_native_actor_task_physically_done(&f.parent));
    signal_host(&f, &f.allow_parent_wake_exit);
    /* Only the scheduler's post-WAKE retirement hook can complete getDone.
     * An in-callback signal alone would be an insufficient physical proof. */
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(ores_native_actor_task_physically_done(&f.parent));
    CHECK(ores_native_actor_task_physically_done(&f.child));
    CHECK(atomic_load(&f.parent_turns) == 2);
    CHECK(atomic_load(&f.child_turns) == 1);
    CHECK(ores_native_actor_scheduler_publish_control(f.scheduler, f.actor, 99));
    CHECK(ores_native_actor_scheduler_drain(f.scheduler));
    CHECK(atomic_load(&f.control_turns) == 1);
    CHECK(ores_native_actor_scheduler_stop(f.scheduler, f.actor));
    CHECK(ores_native_actor_scheduler_wait_done(f.scheduler, f.actor));
    close_fixture(&f);

    Fixture stopped = {0};
    open_fixture(&stopped, 2);
    CHECK(ores_native_actor_scheduler_send(stopped.scheduler, stopped.actor, 1));
    wait_host(&stopped, &stopped.suspended);
    CHECK(atomic_load(&stopped.parent_turns) == 1);
    CHECK(!ores_native_actor_task_physically_done(&stopped.parent));
    CHECK(!ores_native_actor_task_publish(
        &stopped.parent, 1001, ORES_NATIVE_WAKE_READY));
    /* STOP during SUSPENDING revokes the transferred wake, but cannot
     * retire the record while the first guest callback is still on-stack. */
    CHECK(ores_native_actor_task_stop_actor(&stopped.parent));
    CHECK(!ores_native_actor_task_physically_done(&stopped.parent));
    CHECK(!ores_native_actor_scheduler_poll_done(stopped.scheduler, stopped.actor));
    signal_host(&stopped, &stopped.allow_suspend_guest_exit);
    CHECK(ores_native_actor_scheduler_wait_done(stopped.scheduler, stopped.actor));
    CHECK(ores_native_actor_task_physically_done(&stopped.parent));
    CHECK(!ores_native_actor_task_publish(
        &stopped.parent, 1001, ORES_NATIVE_WAKE_READY));
    CHECK(atomic_load(&stopped.parent_turns) == 1);
    CHECK(atomic_load(&stopped.child_turns) == 0);
    close_fixture(&stopped);
    puts("NATIVE_ACTOR_OWNED_TASK_WAKE_PASS");
    return 0;
}
