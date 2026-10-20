#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativePhysicalTurnGate.h"
#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actors[3];
    _Atomic int count;
    _Atomic int errors;
    int observed[4]; /* single worker; published before drain returns */
} TestState;

static void on_data(void* raw, size_t actor, uint64_t payload) {
    TestState* state = (TestState*)raw;
    if (actor > 2 || payload != (uint64_t)(100 + actor)
        || !ores_native_actor_scheduler_is_current_actor(
                state->scheduler, state->actors[actor])) {
        atomic_fetch_add(&state->errors, 1);
    }
    int position = atomic_fetch_add(&state->count, 1);
    if (position >= 0 && position < 4) state->observed[position] = (int)actor;
    else atomic_fetch_add(&state->errors, 1);
}
static void on_control(void* raw, size_t actor, uint64_t payload) {
    TestState* state = (TestState*)raw;
    if (actor != 2 || payload != 777
        || !ores_native_actor_scheduler_is_current_actor(
                state->scheduler, state->actors[actor])) {
        atomic_fetch_add(&state->errors, 1);
    }
    int position = atomic_fetch_add(&state->count, 1);
    if (position >= 0 && position < 4) state->observed[position] = 1000 + (int)actor;
    else atomic_fetch_add(&state->errors, 1);
}
static void gate_available(void* raw) {
    TestState* state = (TestState*)raw;
    ores_native_actor_scheduler_signal_turn_gate(state->scheduler);
}
int main(void) {
    TestState state = {0};
    state.scheduler = ores_native_actor_scheduler_open_control(
            3, 4, 1, on_data, on_control, &state);
    assert(state.scheduler != NULL);
    for (size_t i = 0; i < 3; ++i) {
        state.actors[i] = ores_native_actor_scheduler_spawn(state.scheduler, i);
        assert(state.actors[i].scheduler_domain != 0);
    }
    OresNativePhysicalTurnGate gate = {0};
    gate.maximum = 1;
    gate.notify_free = gate_available;
    gate.context = &state;
    assert(ores_native_actor_scheduler_configure_turn_gate(state.scheduler, &gate));
    /* Hold the one global permit while a worker repeatedly wakes/contends.
     * Speculative ready selection MUST NOT execute a callback or consume mail. */
    assert(ores_native_turn_gate_try_acquire(&gate));
    assert(ores_native_actor_scheduler_send(
            state.scheduler, state.actors[0], 100));
    assert(ores_native_actor_scheduler_send(
            state.scheduler, state.actors[1], 101));
    assert(ores_native_actor_scheduler_publish_control(
            state.scheduler, state.actors[2], 777));
    assert(ores_native_actor_scheduler_send(
            state.scheduler, state.actors[2], 102));
    struct timespec brief = {0, 20000000};
    nanosleep(&brief, NULL);
    assert(atomic_load(&state.count) == 0);
    assert(ores_native_turn_gate_active(&gate) == 1);
    ores_native_turn_gate_release(&gate);
    assert(ores_native_actor_scheduler_drain(state.scheduler));
    assert(atomic_load(&state.errors) == 0);
    assert(atomic_load(&state.count) == 4);
    assert(state.observed[0] == 1002); /* CONTROL before DATA globally */
    assert(state.observed[1] == 100);
    assert(state.observed[2] == 101);
    assert(state.observed[3] == 102);
    assert(ores_native_turn_gate_active(&gate) == 0);
    for (size_t i = 0; i < 3; ++i) {
        assert(ores_native_actor_scheduler_stop(state.scheduler, state.actors[i]));
        assert(ores_native_actor_scheduler_poll_done(state.scheduler, state.actors[i]));
    }
    ores_native_actor_scheduler_retire(state.scheduler);
    assert(ores_native_actor_scheduler_try_destroy(state.scheduler));
    puts("NATIVE_INDEXED_TURN_GATE_PASS priority fifo quiescence");
    return 0;
}
