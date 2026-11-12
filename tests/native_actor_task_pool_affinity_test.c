#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor[2];
    atomic_int active[2];
    atomic_int normal_tasks;
    atomic_int cancelled_tasks;
    atomic_int control_callbacks;
    atomic_int bad;
    atomic_int stop_ready;
    atomic_int release_stop;
    atomic_int nested_admissions;
    atomic_int task_running;
    atomic_int release_task;
} State;

static void assert_actor_turn(State* s, size_t index) {
    assert(index < 2);
    assert(ores_native_actor_scheduler_in_any_actor_turn());
    assert(ores_native_actor_scheduler_is_current_carrier(s->scheduler));
    assert(ores_native_actor_scheduler_is_current_actor(
        s->scheduler, s->actor[index]));
    OresNativeExecutionContext exec = {0};
    assert(ores_native_actor_scheduler_current_execution_context(
        s->scheduler, &exec));
    assert(exec.id.runtime_domain == s->actor[index].scheduler_domain);
    assert(exec.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    assert(exec.allocation.owner == index);
    assert(exec.allocation.generation == s->actor[index].generation);
}

static void on_task(void* opaque, size_t index, uint64_t handle) {
    State* s = (State*)opaque;
    assert_actor_turn(s, index);
    assert(index == 0);
    assert(atomic_load_explicit(&s->control_callbacks, memory_order_acquire) == 1);
    assert((handle >= 1 && handle <= 8) || handle == 101 || handle == 102);
    assert(atomic_fetch_add(&s->active[index], 1) == 0);
    uint64_t sequence = 999;
    assert(!ores_native_actor_scheduler_current_data_mail_sequence(
        s->scheduler, s->actor[index], &sequence));
    assert(sequence == 999);
    if (handle == 1) {
        /* Child is admitted from this Task's own physical actor turn. */
        assert(ores_native_actor_scheduler_try_schedule_task(
            s->scheduler, s->actor[index], 101) == ORES_NATIVE_TASK_ACCEPTED);
        atomic_fetch_add(&s->nested_admissions, 1);
    }
    if (handle == 102) {
        /* STOP may seal while this task remains physically executing.
         * It must not publish DONE until the exact actor lease retires. */
        atomic_store_explicit(&s->task_running, 1, memory_order_release);
        while (!atomic_load_explicit(&s->release_task, memory_order_acquire))
            sched_yield();
    }
    atomic_fetch_add(&s->normal_tasks, 1);
    assert(atomic_fetch_sub(&s->active[index], 1) == 1);
}

static void on_task_cancel(void* opaque, size_t index, uint64_t handle) {
    State* s = (State*)opaque;
    assert(index == 1 && handle == 200);
    assert(ores_native_actor_scheduler_is_current_carrier(s->scheduler));
    assert(ores_native_actor_scheduler_in_any_actor_turn());
    /* STOP only executes a runtime cleanup callback; it may not run guest
     * on_task or admit additional source continuations. */
    OresNativeTaskAdmissionStatus status =
        ores_native_actor_scheduler_try_schedule_task(
            s->scheduler, s->actor[index], 201);
    assert(status != ORES_NATIVE_TASK_ACCEPTED);
    atomic_fetch_add(&s->cancelled_tasks, 1);
}

static void on_data(void* opaque, size_t index, uint64_t payload) {
    State* s = (State*)opaque;
    assert_actor_turn(s, index);
    assert(atomic_fetch_add(&s->active[index], 1) == 0);
    uint64_t sequence = 0;
    assert(ores_native_actor_scheduler_current_data_mail_sequence(
        s->scheduler, s->actor[index], &sequence));
    /* This actor receives a second DATA message in the running-task STOP
     * barrier test. The source-mail ordinal must advance per admitted DATA,
     * while TASK and CONTROL callbacks remain ordinal-free. */
    assert(sequence == (index == 0 && payload == 3 ? 2u : 1u));
    if (index == 0) {
        if (payload == 3) {
            assert(ores_native_actor_scheduler_try_schedule_task(
                s->scheduler, s->actor[0], 102) == ORES_NATIVE_TASK_ACCEPTED);
        } else {
        assert(payload == 1);
        /* Other actors and zero handles are never Task capabilities. */
        assert(ores_native_actor_scheduler_try_schedule_task(
            s->scheduler, s->actor[1], 1)
            == ORES_NATIVE_TASK_FOREIGN_CARRIER);
        assert(ores_native_actor_scheduler_try_schedule_task(
            s->scheduler, s->actor[0], 0)
            == ORES_NATIVE_TASK_INVALID_ACTOR);
        for (uint64_t n = 1; n <= ORES_NATIVE_TASK_SLOTS_PER_ACTOR; ++n)
            assert(ores_native_actor_scheduler_try_schedule_task(
                s->scheduler, s->actor[0], n)
                == ORES_NATIVE_TASK_ACCEPTED);
        assert(ores_native_actor_scheduler_try_schedule_task(
            s->scheduler, s->actor[0], 99)
            == ORES_NATIVE_TASK_MAILBOX_FULL);
        assert(ores_native_actor_scheduler_publish_control(
            s->scheduler, s->actor[0], 99));
        }
    } else {
        assert(index == 1 && payload == 2);
        assert(ores_native_actor_scheduler_try_schedule_task(
            s->scheduler, s->actor[1], 200)
            == ORES_NATIVE_TASK_ACCEPTED);
        atomic_store_explicit(&s->stop_ready, 1, memory_order_release);
        /* Test-only host handoff: stop is requested while this DATA turn
         * still holds its actor lease, before queued Task gets a carrier. */
        while (!atomic_load_explicit(&s->release_stop, memory_order_acquire))
            sched_yield();
    }
    assert(atomic_fetch_sub(&s->active[index], 1) == 1);
}

static void on_control(void* opaque, size_t index, uint64_t payload) {
    State* s = (State*)opaque;
    assert(index == 0 && payload == 99);
    assert_actor_turn(s, index);
    /* CONTROL callbacks have actor identity but NO guest Task admission
     * authority; they must not bypass ordinary actor queue scheduling. */
    assert(ores_native_actor_scheduler_try_schedule_task(
        s->scheduler, s->actor[0], 777)
        == ORES_NATIVE_TASK_FOREIGN_CARRIER);
    atomic_fetch_add_explicit(&s->control_callbacks, 1, memory_order_release);
}

int main(void) {
    State state = {0};
    OresNativeActorSchedulerOptions invalid = {
        .actor_count = 1, .per_actor_capacity = 1, .workers = 1,
        .on_message = on_data, .on_task = on_task, .ctx = &state
    };
    /* Cannot enable guest Task execution without a STOP cleanup callback. */
    assert(!ores_native_actor_scheduler_open_with_options(&invalid));
    OresNativeActorSchedulerOptions options = {
        .actor_count = 2, .per_actor_capacity = 4, .workers = 2,
        .on_message = on_data, .on_control = on_control,
        .on_task = on_task, .on_task_cancel = on_task_cancel,
        .ctx = &state
    };
    state.scheduler = ores_native_actor_scheduler_open_with_options(&options);
    assert(state.scheduler);
    state.actor[0] = ores_native_actor_scheduler_spawn(state.scheduler, 0);
    state.actor[1] = ores_native_actor_scheduler_spawn(state.scheduler, 1);
    assert(state.actor[0].scheduler_domain != 0);
    assert(state.actor[1].scheduler_domain == state.actor[0].scheduler_domain);
    assert(!ores_native_actor_scheduler_in_any_actor_turn());
    assert(ores_native_actor_scheduler_try_schedule_task(
        state.scheduler, state.actor[0], 100)
        == ORES_NATIVE_TASK_FOREIGN_CARRIER);
    assert(ores_native_actor_scheduler_try_send(
        state.scheduler, state.actor[0], 1) == ORES_NATIVE_SEND_ACCEPTED);
    assert(ores_native_actor_scheduler_try_send(
        state.scheduler, state.actor[1], 2) == ORES_NATIVE_SEND_ACCEPTED);

    struct timespec start;
    assert(timespec_get(&start, TIME_UTC) == TIME_UTC);
    /* No unbounded waiting for the actor START admission witness. */
    for (;;) {
        if (atomic_load_explicit(&state.stop_ready, memory_order_acquire))
            break;
        struct timespec now;
        assert(timespec_get(&now, TIME_UTC) == TIME_UTC);
        assert(now.tv_sec - start.tv_sec < 15);
        sched_yield();
    }
    assert(ores_native_actor_scheduler_stop_sealed(
        state.scheduler, state.actor[1]));
    atomic_store_explicit(&state.release_stop, 1, memory_order_release);

    assert(ores_native_actor_scheduler_drain(state.scheduler));
    assert(ores_native_actor_scheduler_wait_done(
        state.scheduler, state.actor[1]));
    assert(atomic_load(&state.normal_tasks) == 9);
    assert(atomic_load(&state.nested_admissions) == 1);
    assert(atomic_load(&state.cancelled_tasks) == 1);
    assert(atomic_load(&state.control_callbacks) == 1);
    assert(atomic_load(&state.active[0]) == 0);
    assert(atomic_load(&state.active[1]) == 0);
    /* A running Task cannot be discarded as if it were merely queued.
     * A STOP must be nonblocking, but DONE must wait for physical quiescence. */
    assert(ores_native_actor_scheduler_try_send(
        state.scheduler, state.actor[0], 3) == ORES_NATIVE_SEND_ACCEPTED);
    assert(timespec_get(&start, TIME_UTC) == TIME_UTC);
    for (;;) {
        if (atomic_load_explicit(&state.task_running, memory_order_acquire)) break;
        struct timespec now;
        assert(timespec_get(&now, TIME_UTC) == TIME_UTC);
        assert(now.tv_sec - start.tv_sec < 15);
        sched_yield();
    }
    assert(ores_native_actor_scheduler_stop_sealed(state.scheduler, state.actor[0]));
    assert(!ores_native_actor_scheduler_poll_done(state.scheduler, state.actor[0]));
    atomic_store_explicit(&state.release_task, 1, memory_order_release);
    assert(ores_native_actor_scheduler_wait_done(
        state.scheduler, state.actor[0]));
    assert(atomic_load(&state.normal_tasks) == 10);
    assert(!ores_native_actor_scheduler_in_any_actor_turn());
    ores_native_actor_scheduler_retire(state.scheduler);
    assert(ores_native_actor_scheduler_try_destroy(state.scheduler));
    printf("NATIVE_ACTOR_TASK_POOL_PASS normal=10 cancel=1 nested=1 control=1\n");
    return 0;
}
