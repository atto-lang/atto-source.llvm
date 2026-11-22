#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int ok) { if (!ok) abort(); }

typedef struct {
    OresNativeActorScheduler* scheduler;
    atomic_uint_fast64_t seen_execution;
    atomic_int errors;
} State;

static void run(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    OresNativeExecutionContext context = {0};
    if (actor != 0 || payload != 7
            || !ores_native_actor_scheduler_current_execution_context(
                    s->scheduler, &context)
            || context.id.execution != UINT64_MAX
            || context.allocation.kind != ORES_ALLOCATION_DOMAIN_ACTOR
            || context.allocation.owner != 0) {
        atomic_fetch_add(&s->errors, 1);
        return;
    }
    atomic_store(&s->seen_execution, context.id.execution);
}

int main(void) {
    State state = {0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(1, 4, 1, run, &state);
    check(scheduler != NULL);
    state.scheduler = scheduler;

    OresNativeActorRef actor = ores_native_actor_scheduler_spawn(scheduler, 0);
    check(actor.scheduler_domain != 0);
    check(ores_native_actor_scheduler_test_set_next_execution(
        scheduler, UINT64_MAX));

    check(ores_native_actor_scheduler_try_send(scheduler, actor, 7)
        == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_actor_scheduler_try_send(scheduler, actor, 8)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);

    check(ores_native_actor_scheduler_drain(scheduler));
    check(atomic_load(&state.errors) == 0);
    check(atomic_load(&state.seen_execution) == UINT64_MAX);

    /* Exhaustion remains fail-closed after the final reserved id executes. */
    check(ores_native_actor_scheduler_try_send(scheduler, actor, 9)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    check(ores_native_actor_scheduler_accepted(scheduler) == 1);
    check(ores_native_actor_scheduler_rejected(scheduler) == 2);

    check(ores_native_actor_scheduler_stop(scheduler, actor));
    check(ores_native_actor_scheduler_poll_done(scheduler, actor));
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_try_destroy(scheduler));

    puts("NATIVE_EXECUTION_CONTEXT_EXHAUSTION_PASS");
    return 0;
}
