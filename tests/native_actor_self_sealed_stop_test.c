#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    atomic_int entered;
    atomic_int release;
    atomic_int callbacks;
    atomic_int errors;
    atomic_int stopped;
} SelfStop;

static void check(int valid) { if (!valid) abort(); }
static void handler(void* opaque, size_t actor_slot, uint64_t message) {
    SelfStop* state = (SelfStop*)opaque;
    if (actor_slot != 0 || message != 0
            || !ores_native_actor_scheduler_is_current_actor(
                state->scheduler, state->actor))
        atomic_fetch_add(&state->errors, 1);
    atomic_fetch_add(&state->callbacks, 1);
    atomic_store_explicit(&state->entered, 1, memory_order_release);
    while (!atomic_load_explicit(&state->release, memory_order_acquire))
        sched_yield();

    /* These calls are forbidden from this scheduler's own worker. A blocking
     * wait here would deadlock a single-flight, non-reentrant actor. */
    if (ores_native_actor_scheduler_wait_done(state->scheduler, state->actor)
            || ores_native_actor_scheduler_drain(state->scheduler)
            || ores_native_actor_scheduler_try_destroy(state->scheduler))
        atomic_fetch_add(&state->errors, 1);

    if (!ores_native_actor_scheduler_stop_sealed(
            state->scheduler, state->actor)
            || ores_native_actor_scheduler_stop_sealed(
                state->scheduler, state->actor))
        atomic_fetch_add(&state->errors, 1);
    if (ores_native_actor_scheduler_try_send(
            state->scheduler, state->actor, 999)
            != ORES_NATIVE_SEND_TERMINATED)
        atomic_fetch_add(&state->errors, 1);
    atomic_store_explicit(&state->stopped, 1, memory_order_release);
}

int main(void) {
    SelfStop state = {0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(1, 8, 4, handler, &state);
    check(scheduler != NULL);
    state.scheduler = scheduler;
    state.actor = ores_native_actor_scheduler_spawn(scheduler, 0);
    check(state.actor.scheduler_domain != 0);
    check(ores_native_actor_scheduler_try_send(
        scheduler, state.actor, 0) == ORES_NATIVE_SEND_ACCEPTED);

    /* Bounded rendezvous avoids an unbounded busy-wait on a broken runtime. */
    int entered = 0;
    const struct timespec delay = {0, 1000000};
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load_explicit(&state.entered, memory_order_acquire)) {
            entered = 1;
            break;
        }
        nanosleep(&delay, NULL);
    }
    check(entered);
    for (uint64_t message = 1; message <= 7; ++message)
        check(ores_native_actor_scheduler_try_send(
            scheduler, state.actor, message) == ORES_NATIVE_SEND_ACCEPTED);
    check(!ores_native_actor_scheduler_poll_done(scheduler, state.actor));
    atomic_store_explicit(&state.release, 1, memory_order_release);

    /* wait_done only waits for an actor whose STOP/retirement is already
     * recorded (it returns 0 otherwise), so first observe the handler's
     * sealed STOP, again with a bounded rendezvous. */
    int stopped = 0;
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load_explicit(&state.stopped, memory_order_acquire)) {
            stopped = 1;
            break;
        }
        nanosleep(&delay, NULL);
    }
    check(stopped);
    check(ores_native_actor_scheduler_wait_done(scheduler, state.actor));
    check(atomic_load(&state.errors) == 0);
    check(atomic_load(&state.callbacks) == 1);
    check(ores_native_actor_scheduler_poll_done(scheduler, state.actor));
    OresNativeActorTerminalState terminal = {0};
    check(ores_native_actor_scheduler_terminal_state(
        scheduler, state.actor, &terminal));
    check(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    check(terminal.accepted_user_messages == 8);
    check(terminal.processed_user_messages == 1);
    check(terminal.abandoned_user_messages == 7);
    check(terminal.abandoned_control_messages == 0);
    check(terminal.accepted_user_messages ==
        terminal.processed_user_messages + terminal.abandoned_user_messages);
    check(ores_native_actor_scheduler_accepted(scheduler) == 8);
    check(ores_native_actor_scheduler_completed(scheduler) == 1);
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(ores_native_actor_scheduler_try_destroy(scheduler));
    puts("NATIVE_ACTOR_SELF_SEALED_STOP_PASS worker_wait_denied=1 callbacks=1 abandoned=7 stale_send_denied=1");
    return 0;
}
