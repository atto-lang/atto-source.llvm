#include "oreslang/runtime/NativeThreadJoinCarrierGuardV1.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    atomic_int release;
    atomic_int drops;
} ThreadGate;

typedef struct {
    ThreadGate* gate;
    int value;
} ThreadArg;

static int entry(void* raw) {
    ThreadArg* arg = (ThreadArg*)raw;
    while (!atomic_load_explicit(&arg->gate->release, memory_order_acquire))
        sched_yield(); /* physical pthread, NOT a scheduler carrier */
    return arg->value;
}

static void drop_arg(void* raw) {
    ThreadArg* arg = (ThreadArg*)raw;
    atomic_fetch_add_explicit(&arg->gate->drops, 1, memory_order_acq_rel);
    free(arg);
}

typedef struct {
    OresNativeActorScheduler* scheduler;
    ores_native_thread_v1* handle; /* host retains ownership */
    atomic_int callbacks;
} ActorState;

static void actor_turn(void* raw, size_t lane, uint64_t payload) {
    ActorState* state = (ActorState*)raw;
    assert(lane == 0 && payload == 1);
    assert(ores_native_actor_scheduler_is_any_current_carrier());
    assert(ores_native_actor_scheduler_is_current_carrier(state->scheduler));

    int value = -999;
    /* The native pthread cannot finish yet; this MUST return immediately. */
    assert(ores_native_thread_join_blocking_checked_v1(state->handle, &value)
           == ORES_THREAD_CARRIER_WOULD_BLOCK_V1);
    assert(value == -999); /* no accidental consumption or partial result */
    atomic_fetch_add_explicit(&state->callbacks, 1, memory_order_release);
}

static void await_callback(ActorState* state) {
    const time_t deadline = time(NULL) + 8;
    while (!atomic_load_explicit(&state->callbacks, memory_order_acquire)) {
        assert(time(NULL) <= deadline);
        sched_yield(); /* external test host only */
    }
}

int main(void) {
    assert(!ores_native_actor_scheduler_is_any_current_carrier());

    int value = -100;
    assert(ores_native_thread_join_blocking_checked_v1(NULL, &value)
           == ORES_THREAD_BAD_ARGUMENT_V1);
    assert(value == -100);

    ThreadGate gate = {0};
    ThreadArg* arg = (ThreadArg*)malloc(sizeof(*arg));
    assert(arg);
    *arg = (ThreadArg){&gate, 91};
    ores_native_thread_v1* handle = NULL;
    assert(ores_native_thread_spawn_v1(entry, arg, drop_arg, &handle)
           == ORES_THREAD_OK_V1);
    assert(handle);

    /* Two distinct scheduler instances must both reject physical join.
     * Passing one scheduler's pointer is not sufficient for this check. */
    ActorState states[2] = {
        {.scheduler = NULL, .handle = handle, .callbacks = 0},
        {.scheduler = NULL, .handle = handle, .callbacks = 0}
    };
    OresNativeActorRef actors[2];
    for (int i = 0; i < 2; ++i) {
        states[i].scheduler = ores_native_actor_scheduler_open(
            1, 2, 1, actor_turn, &states[i]);
        assert(states[i].scheduler);
        actors[i] = ores_native_actor_scheduler_spawn(states[i].scheduler, 0);
        assert(actors[i].scheduler_domain);
        assert(ores_native_actor_scheduler_try_send(
            states[i].scheduler, actors[i], 1) == ORES_NATIVE_SEND_ACCEPTED);
    }

    for (int i = 0; i < 2; ++i) {
        await_callback(&states[i]);
        assert(ores_native_actor_scheduler_drain(states[i].scheduler));
    }

    int completed = -1;
    assert(ores_native_thread_poll_completed_v1(handle, &completed)
           == ORES_THREAD_OK_V1);
    assert(completed == 0); /* scheduler calls never physically joined it */
    atomic_store_explicit(&gate.release, 1, memory_order_release);

    assert(!ores_native_actor_scheduler_is_any_current_carrier());
    assert(ores_native_thread_join_blocking_checked_v1(handle, &value)
           == ORES_THREAD_OK_V1);
    assert(value == 91);
    assert(ores_native_thread_release_v1(handle) == ORES_THREAD_OK_V1);
    assert(atomic_load_explicit(&gate.drops, memory_order_acquire) == 1);

    for (int i = 0; i < 2; ++i) {
        assert(ores_native_actor_scheduler_stop(states[i].scheduler, actors[i]));
        assert(ores_native_actor_scheduler_wait_done(
            states[i].scheduler, actors[i]));
        ores_native_actor_scheduler_retire(states[i].scheduler);
        assert(ores_native_actor_scheduler_drain(states[i].scheduler));
        assert(ores_native_actor_scheduler_try_destroy(states[i].scheduler));
    }
    puts("NATIVE_THREAD_JOIN_CARRIER_GUARD_HOST_ONLY_PASS");
    return 0;
}
