#include "oreslang/runtime/NativeActorScheduler.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    atomic_int entered;
    atomic_int release;
    atomic_int delivered;
    atomic_int controls;
} State;

static void check(int ok) {
    if (!ok) abort();
}

static void handler(void* opaque, size_t actor_index, uint64_t payload) {
    State* state = (State*)opaque;
    check(actor_index == 0);
    if (payload == 1) {
        atomic_store_explicit(&state->entered, 1, memory_order_release);
        while (!atomic_load_explicit(&state->release, memory_order_acquire))
            sched_yield();
    }
    atomic_fetch_add_explicit(&state->delivered, 1, memory_order_acq_rel);
}

static void control(void* opaque, size_t actor_index, uint64_t payload) {
    State* state = (State*)opaque;
    check(actor_index == 0);
    check(payload == UINT64_C(99));
    atomic_fetch_add_explicit(&state->controls, 1, memory_order_acq_rel);
}

static void run_case(int sealed) {
    State state = {0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(1, 4, 1, handler, &state);
    check(scheduler != NULL);

    OresNativeActorRef actor =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    check(actor.scheduler_domain != 0);

    check(ores_native_actor_scheduler_try_send(scheduler, actor, 1)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (!atomic_load_explicit(&state.entered, memory_order_acquire))
        sched_yield();

    check(ores_native_actor_scheduler_try_send(scheduler, actor, 2)
        == ORES_NATIVE_SEND_ACCEPTED);

    if (sealed)
        check(ores_native_actor_scheduler_stop_sealed(scheduler, actor));
    else
        check(ores_native_actor_scheduler_stop(scheduler, actor));

    check(ores_native_actor_scheduler_try_send(scheduler, actor, 3)
        == ORES_NATIVE_SEND_TERMINATED);
    check(!ores_native_actor_scheduler_poll_done(scheduler, actor));

    atomic_store_explicit(&state.release, 1, memory_order_release);
    check(ores_native_actor_scheduler_wait_done(scheduler, actor));

    OresNativeActorTerminalState terminal = {0};
    check(ores_native_actor_scheduler_terminal_state(
        scheduler, actor, &terminal));
    check(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    check(terminal.accepted_user_messages == 2);
    check(terminal.processed_user_messages == (sealed ? 1u : 2u));
    check(terminal.abandoned_user_messages == (sealed ? 1u : 0u));
    check(terminal.processed_user_messages + terminal.abandoned_user_messages
        == terminal.accepted_user_messages);
    check(terminal.abandoned_control_messages == 0);
    check(atomic_load_explicit(&state.delivered, memory_order_acquire)
        == (sealed ? 1 : 2));

    check(ores_native_actor_scheduler_accepted(scheduler) == 2);
    check(ores_native_actor_scheduler_completed(scheduler)
        == (sealed ? 1u : 2u));

    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void run_resource_loan_case(void) {
    State state = {0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(1, 2, 1, handler, &state);
    check(scheduler != NULL);

    OresNativeActorRef actor =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    check(actor.scheduler_domain != 0);
    check(ores_native_actor_scheduler_retain_resource(scheduler, actor));

    /* STOP seals new resource admission but does not strand the loan which
     * already won the scheduler-lock race. DONE remains physically fenced. */
    check(ores_native_actor_scheduler_stop_sealed(scheduler, actor));
    check(!ores_native_actor_scheduler_retain_resource(scheduler, actor));
    check(!ores_native_actor_scheduler_poll_done(scheduler, actor));

    OresNativeActorRef stale = actor;
    ++stale.generation;
    check(!ores_native_actor_scheduler_release_resource(scheduler, stale));
    check(!ores_native_actor_scheduler_poll_done(scheduler, actor));

    check(ores_native_actor_scheduler_release_resource(scheduler, actor));
    check(!ores_native_actor_scheduler_release_resource(scheduler, actor));
    check(ores_native_actor_scheduler_poll_done(scheduler, actor));
    check(ores_native_actor_scheduler_wait_done(scheduler, actor));

    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void run_control_case(void) {
    State state = {0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open_control(
            1, 4, 1, handler, control, &state);
    check(scheduler != NULL);

    OresNativeActorRef actor =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    check(actor.scheduler_domain != 0);

    check(ores_native_actor_scheduler_try_send(scheduler, actor, 1)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (!atomic_load_explicit(&state.entered, memory_order_acquire))
        sched_yield();

    check(ores_native_actor_scheduler_try_send(scheduler, actor, 2)
        == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_actor_scheduler_publish_control(
        scheduler, actor, UINT64_C(99)));

    check(ores_native_actor_scheduler_stop_sealed(scheduler, actor));
    atomic_store_explicit(&state.release, 1, memory_order_release);
    check(ores_native_actor_scheduler_wait_done(scheduler, actor));

    OresNativeActorTerminalState terminal = {0};
    check(ores_native_actor_scheduler_terminal_state(
        scheduler, actor, &terminal));
    check(terminal.accepted_user_messages == 2);
    check(terminal.processed_user_messages == 1);
    check(terminal.abandoned_user_messages == 1);
    check(terminal.abandoned_control_messages == 0);
    check(atomic_load_explicit(&state.delivered, memory_order_acquire) == 1);
    check(atomic_load_explicit(&state.controls, memory_order_acquire) == 1);
    check(ores_native_actor_scheduler_accepted(scheduler) == 3);
    check(ores_native_actor_scheduler_completed(scheduler) == 2);
    check(ores_native_actor_scheduler_control_completed(scheduler) == 1);

    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(ores_native_actor_scheduler_try_destroy(scheduler));
}

int main(void) {
    run_case(1);
    run_case(0);
    run_resource_loan_case();
    run_control_case();
    puts("NATIVE_ACTOR_SEALED_STOP_PASS");
    return 0;
}
