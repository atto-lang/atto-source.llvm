#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

/* The first actor keeps its sole carrier occupied. Fault-injected failure
 * to start a second carrier must reject independent work without enqueueing
 * it, and a later successful retry must execute before the first unblocks. */
typedef struct {
    atomic_int entered;
    atomic_int release;
    atomic_int other_executed;
} State;
static void require(int ok) { if (!ok) abort(); }
static void deliver(void* opaque, size_t actor, uint64_t message) {
    State* s = (State*)opaque;
    if (actor == 0) {
        require(message == 100);
        atomic_store(&s->entered, 1);
        while (!atomic_load(&s->release)) sched_yield();
    } else {
        require(actor == 1 && message == 200);
        atomic_fetch_add(&s->other_executed, 1);
    }
}
static void until(atomic_int* flag) {
    for (int spins = 0; spins < 2000000 && !atomic_load(flag); ++spins)
        sched_yield();
    require(atomic_load(flag) != 0);
}
int main(void) {
    State state = {0};
    OresNativeActorScheduler* s =
        ores_native_actor_scheduler_open(2, 2, 2, deliver, &state);
    require(s != NULL);
    OresNativeActorRef first = ores_native_actor_scheduler_spawn(s, 0);
    OresNativeActorRef second = ores_native_actor_scheduler_spawn(s, 1);
    require(first.scheduler_domain && second.scheduler_domain);
    require(ores_native_actor_scheduler_started_carriers(s) == 0);

    /* The first required carrier fails: no ghost queued task, no carrier. */
    ores_native_actor_scheduler_test_fail_next_create();
    require(ores_native_actor_scheduler_try_send(s, first, 100)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    require(ores_native_actor_scheduler_started_carriers(s) == 0);
    require(ores_native_actor_scheduler_accepted(s) == 0);
    require(ores_native_actor_scheduler_rejected(s) == 1);

    /* Legacy index send must also count a failed creation exactly once. */
    ores_native_actor_scheduler_test_fail_next_create();
    require(!ores_native_actor_scheduler_submit(s, 0, 100));
    require(ores_native_actor_scheduler_rejected(s) == 2);
    require(ores_native_actor_scheduler_accepted(s) == 0);

    require(ores_native_actor_scheduler_try_send(s, first, 100)
        == ORES_NATIVE_SEND_ACCEPTED);
    until(&state.entered);
    require(ores_native_actor_scheduler_started_carriers(s) == 1);

    /* A live but occupied first carrier is not available for actor 1. */
    ores_native_actor_scheduler_test_fail_next_create();
    require(ores_native_actor_scheduler_try_send(s, second, 200)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    require(ores_native_actor_scheduler_accepted(s) == 1);
    require(ores_native_actor_scheduler_rejected(s) == 3);
    require(ores_native_actor_scheduler_started_carriers(s) == 1);
    require(atomic_load(&state.other_executed) == 0);

    require(ores_native_actor_scheduler_try_send(s, second, 200)
        == ORES_NATIVE_SEND_ACCEPTED);
    until(&state.other_executed);
    require(!atomic_load(&state.release));
    require(atomic_load(&state.other_executed) == 1);
    require(ores_native_actor_scheduler_started_carriers(s) == 2);
    require(ores_native_actor_scheduler_accepted(s) == 2);

    require(ores_native_actor_scheduler_stop(s, first));
    require(ores_native_actor_scheduler_stop(s, second));
    ores_native_actor_scheduler_retire(s);
    atomic_store(&state.release, 1);
    require(ores_native_actor_scheduler_drain(s));
    require(ores_native_actor_scheduler_completed(s) == 2);
    require(ores_native_actor_scheduler_rejected(s) == 3);
    require(ores_native_actor_scheduler_try_destroy(s));
    return 0;
}
