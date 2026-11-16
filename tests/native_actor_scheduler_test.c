#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#define ACTORS 5
#define EACH 200
static void check(int ok) { if (!ok) abort(); }
typedef struct {
    atomic_int active[ACTORS], peak[ACTORS], next[ACTORS], total, errors;
    atomic_int global_active, global_peak;
    atomic_uint_fast64_t last_execution[ACTORS];
    atomic_uint_fast64_t allocation_domain[ACTORS];
    atomic_uint_fast64_t allocation_generation[ACTORS];
    atomic_int hold;
    OresNativeActorScheduler* scheduler;
} State;
static void run(void* raw, size_t actor, uint64_t msg) {
    State* s = (State*)raw;
    OresNativeExecutionContext execution = {0};
    if (!s->scheduler
            || !ores_native_actor_scheduler_current_execution_context(
                    s->scheduler, &execution)
            || execution.id.runtime_domain == 0
            || execution.id.execution == 0
            || execution.allocation.kind != ORES_ALLOCATION_DOMAIN_ACTOR
            || execution.allocation.domain != execution.id.runtime_domain
            || execution.allocation.owner != actor
            || execution.allocation.generation == 0
            || ores_native_store_handle_valid(execution.parent_execution)
            || ores_native_store_handle_valid(execution.continuation)) {
        atomic_fetch_add(&s->errors, 1);
    } else {
        uint64_t previous = atomic_exchange(
                &s->last_execution[actor], execution.id.execution);
        if (previous && execution.id.execution <= previous)
            atomic_fetch_add(&s->errors, 1);

        uint64_t expected_domain = 0;
        if (!atomic_compare_exchange_strong(
                &s->allocation_domain[actor],
                &expected_domain,
                execution.allocation.domain)
                && expected_domain != execution.allocation.domain)
            atomic_fetch_add(&s->errors, 1);

        uint64_t expected_generation = 0;
        if (!atomic_compare_exchange_strong(
                &s->allocation_generation[actor],
                &expected_generation,
                execution.allocation.generation)
                && expected_generation != execution.allocation.generation)
            atomic_fetch_add(&s->errors, 1);
    }
    int current = atomic_fetch_add(&s->active[actor], 1) + 1;
    int previous = atomic_load(&s->peak[actor]);
    while (current > previous &&
           !atomic_compare_exchange_weak(&s->peak[actor], &previous, current)) { }
    int global_current = atomic_fetch_add(&s->global_active, 1) + 1;
    int global_previous = atomic_load(&s->global_peak);
    while (global_current > global_previous &&
           !atomic_compare_exchange_weak(
               &s->global_peak, &global_previous, global_current)) { }
    if (current != 1 || (int)msg != atomic_fetch_add(&s->next[actor], 1))
        atomic_fetch_add(&s->errors, 1);
    while (atomic_load(&s->hold)) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    atomic_fetch_sub(&s->active[actor], 1);
    atomic_fetch_sub(&s->global_active, 1);
    atomic_fetch_add(&s->total, 1);
}
/* A far-end runnable slot amid thousands of idle lanes must still dispatch. */
static void run_far_slot(void* raw, size_t actor, uint64_t message) {
    if (actor != 4096u || message != 7u) abort();
    atomic_fetch_add_explicit((atomic_int*)raw, 1, memory_order_relaxed);
}
int main(void) {
    State s = {0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(ACTORS, EACH, 8, run, &s);
    check(scheduler != NULL);
    check(ores_native_actor_scheduler_started_carriers(scheduler) == 0);
    check(ores_native_actor_scheduler_prestart_carriers(scheduler));
    check(ores_native_actor_scheduler_all_carriers_started(scheduler));
    check(ores_native_actor_scheduler_started_carriers(scheduler) == 8);
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT >= 0
    check(ores_native_actor_scheduler_priority_inheritance_enabled(scheduler));
#else
    check(!ores_native_actor_scheduler_priority_inheritance_enabled(scheduler));
#endif
    s.scheduler = scheduler;
    OresNativeExecutionContext outside = {0};
    check(!ores_native_actor_scheduler_current_execution_context(scheduler, &outside));
    OresNativeActorRef refs[ACTORS];
    for (int a = 0; a < ACTORS; ++a) {
        refs[a] = ores_native_actor_scheduler_spawn(scheduler, (size_t)a);
        check(refs[a].scheduler_domain && ores_native_actor_scheduler_poll_ready(scheduler, refs[a]));
        OresNativeActorTerminalState premature = {0};
        check(!ores_native_actor_scheduler_terminal_state(
            scheduler, refs[a], &premature));
        check(!ores_native_actor_scheduler_spawn(scheduler, (size_t)a).scheduler_domain);
    }
    for (int n = 0; n < EACH; ++n)
        for (size_t a = 0; a < ACTORS; ++a)
            check(ores_native_actor_scheduler_send(scheduler, refs[a], (uint64_t)n));
    check(ores_native_actor_scheduler_drain(scheduler));
    check(atomic_load(&s.total) == ACTORS * EACH);
    check(atomic_load(&s.errors) == 0);
    for (int a = 0; a < ACTORS; ++a)
        check(atomic_load(&s.peak[a]) == 1);
    check(ores_native_actor_scheduler_accepted(scheduler) == ACTORS * EACH);
    check(ores_native_actor_scheduler_rejected(scheduler) == 0);
    for (int a = 0; a < ACTORS; ++a) {
        check(ores_native_actor_scheduler_stop(scheduler, refs[a]));
        check(ores_native_actor_scheduler_poll_done(scheduler, refs[a]));
        check(!ores_native_actor_scheduler_poll_ready(scheduler, refs[a]));
        check(!ores_native_actor_scheduler_send(scheduler, refs[a], 999));

        OresNativeActorTerminalState terminal = {0};
        OresNativeActorTerminalState repeated = {0};
        check(ores_native_actor_scheduler_terminal_state(
            scheduler, refs[a], &terminal));
        check(terminal.actor.scheduler_domain == refs[a].scheduler_domain);
        check(terminal.actor.actor_slot == refs[a].actor_slot);
        check(terminal.actor.generation == refs[a].generation);
        check(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
        check(terminal.accepted_user_messages == EACH);
        check(terminal.processed_user_messages == EACH);
        check(terminal.abandoned_user_messages == 0);
        check(terminal.abandoned_control_messages == 0);
        check(ores_native_actor_scheduler_terminal_state(
            scheduler, refs[a], &repeated));
        check(repeated.kind == terminal.kind);
        check(repeated.accepted_user_messages == terminal.accepted_user_messages);
        check(repeated.processed_user_messages == terminal.processed_user_messages);

        OresNativeActorRef stale_terminal = refs[a];
        ++stale_terminal.generation;
        check(!ores_native_actor_scheduler_terminal_state(
            scheduler, stale_terminal, &repeated));
    }
    ores_native_actor_scheduler_retire(scheduler);
    check(!ores_native_actor_scheduler_submit(scheduler, 0, 999));
    check(ores_native_actor_scheduler_try_destroy(scheduler));

    /*
     * Physical carrier parallelism is cross-actor capacity only. Two distinct
     * actor domains may execute simultaneously, while run() still asserts each
     * individual actor has active==1. This must remain true until compiler-
     * proven per-turn actor-state isolation enables same-actor parallelism.
     */
    State parallel = {0};
    atomic_store(&parallel.hold, 1);
    scheduler = ores_native_actor_scheduler_open(2, 1, 2, run, &parallel);
    check(scheduler != NULL);
    parallel.scheduler = scheduler;
    check(ores_native_actor_scheduler_prestart_carriers(scheduler));
    check(ores_native_actor_scheduler_started_carriers(scheduler) == 2);
    OresNativeActorRef parallel_a =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    OresNativeActorRef parallel_b =
        ores_native_actor_scheduler_spawn(scheduler, 1);
    check(parallel_a.scheduler_domain && parallel_b.scheduler_domain);
    check(ores_native_actor_scheduler_send(scheduler, parallel_a, 0));
    check(ores_native_actor_scheduler_send(scheduler, parallel_b, 0));

    struct timespec overlap_pause = {0, 1000000};
    int observed_overlap = 0;
    for (int attempt = 0; attempt < 5000; ++attempt) {
        if (atomic_load(&parallel.global_active) == 2) {
            observed_overlap = 1;
            break;
        }
        nanosleep(&overlap_pause, NULL);
    }
    check(observed_overlap);
    check(atomic_load(&parallel.peak[0]) == 1);
    check(atomic_load(&parallel.peak[1]) == 1);
    check(atomic_load(&parallel.global_peak) == 2);

    atomic_store(&parallel.hold, 0);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(atomic_load(&parallel.total) == 2);
    check(atomic_load(&parallel.errors) == 0);
    check(ores_native_actor_scheduler_stop(scheduler, parallel_a));
    check(ores_native_actor_scheduler_stop(scheduler, parallel_b));
    check(ores_native_actor_scheduler_poll_done(scheduler, parallel_a));
    check(ores_native_actor_scheduler_poll_done(scheduler, parallel_b));
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_try_destroy(scheduler));

    State pressure = {0};
    atomic_store(&pressure.hold, 1);
    scheduler = ores_native_actor_scheduler_open(1, 1, 2, run, &pressure);
    check(scheduler != NULL);
    pressure.scheduler = scheduler;
    OresNativeActorRef one = ores_native_actor_scheduler_spawn(scheduler, 0);
    check(one.scheduler_domain != 0);
    check(ores_native_actor_scheduler_try_send(scheduler, refs[0], 0)
          == ORES_NATIVE_SEND_INVALID_REF); /* foreign domain */
    OresNativeActorRef stale = one;
    stale.generation++;
    check(ores_native_actor_scheduler_try_send(scheduler, stale, 0)
          == ORES_NATIVE_SEND_INVALID_REF);
    check(ores_native_actor_scheduler_try_send(scheduler, one, 0)
          == ORES_NATIVE_SEND_ACCEPTED);
    /* The admitted or queued first item consumes the full capacity. */
    check(ores_native_actor_scheduler_try_send(scheduler, one, 1)
          == ORES_NATIVE_SEND_MAILBOX_FULL);
    check(!ores_native_actor_scheduler_submit(scheduler, 9, 1));
    check(ores_native_actor_scheduler_accepted(scheduler) == 1);
    check(ores_native_actor_scheduler_rejected(scheduler) == 4);
    check(ores_native_actor_scheduler_stop(scheduler, one));
    check(ores_native_actor_scheduler_try_send(scheduler, one, 2)
          == ORES_NATIVE_SEND_TERMINATED);
    check(!ores_native_actor_scheduler_poll_ready(scheduler, one));
    check(!ores_native_actor_scheduler_poll_done(scheduler, one));
    OresNativeActorTerminalState pressure_terminal = {0};
    check(!ores_native_actor_scheduler_terminal_state(
        scheduler, one, &pressure_terminal));
    ores_native_actor_scheduler_retire(scheduler);
    check(!ores_native_actor_scheduler_try_destroy(scheduler));
    atomic_store(&pressure.hold, 0);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(ores_native_actor_scheduler_completed(scheduler) == 1);
    check(ores_native_actor_scheduler_poll_done(scheduler, one));
    check(ores_native_actor_scheduler_terminal_state(
        scheduler, one, &pressure_terminal));
    check(pressure_terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
    check(pressure_terminal.accepted_user_messages == 1);
    check(pressure_terminal.processed_user_messages == 1);
    check(pressure_terminal.abandoned_user_messages == 0);
    check(pressure_terminal.abandoned_control_messages == 0);
    check(ores_native_actor_scheduler_try_destroy(scheduler));

    /* Scheduler-wide retirement has a distinct retained classification. */
    State runtime_retire = {0};
    scheduler = ores_native_actor_scheduler_open(1, 4, 1, run, &runtime_retire);
    check(scheduler != NULL);
    OresNativeActorRef runtime_actor =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    check(runtime_actor.scheduler_domain != 0);
    for (uint64_t value = 0; value < 3; ++value)
        check(ores_native_actor_scheduler_try_send(
            scheduler, runtime_actor, value) == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_actor_scheduler_drain(scheduler));
    OresNativeActorTerminalState runtime_terminal = {0};
    check(!ores_native_actor_scheduler_terminal_state(
        scheduler, runtime_actor, &runtime_terminal));
    ores_native_actor_scheduler_retire(scheduler);
    check(ores_native_actor_scheduler_poll_done(scheduler, runtime_actor));
    check(ores_native_actor_scheduler_terminal_state(
        scheduler, runtime_actor, &runtime_terminal));
    check(runtime_terminal.kind == ORES_NATIVE_TERMINATION_RUNTIME_RETIRED);
    check(runtime_terminal.accepted_user_messages == 3);
    check(runtime_terminal.processed_user_messages == 3);
    check(runtime_terminal.abandoned_user_messages == 0);
    check(runtime_terminal.abandoned_control_messages == 0);
    check(ores_native_actor_scheduler_try_destroy(scheduler));
    /* The prior bitmap branch raised the actor ceiling to 1M. Preserve that
     * explicit capacity goal while keeping main's O(1) intrusive queues. */
    atomic_int far_seen;
    atomic_init(&far_seen, 0);
    OresNativeActorScheduler* large =
        ores_native_actor_scheduler_open(4097, 1, 1, run_far_slot, &far_seen);
    check(large != NULL);
    OresNativeActorRef far = ores_native_actor_scheduler_spawn(large, 4096);
    check(far.scheduler_domain != 0);
    check(ores_native_actor_scheduler_send(large, far, 7));
    check(ores_native_actor_scheduler_drain(large));
    check(atomic_load(&far_seen) == 1);
    check(ores_native_actor_scheduler_stop(large, far));
    ores_native_actor_scheduler_retire(large);
    check(ores_native_actor_scheduler_try_destroy(large));
    check(ores_native_actor_scheduler_open(
        (size_t)ORES_NATIVE_SCHEDULER_MAX_ACTORS + 1u,
        1, 1, run_far_slot, &far_seen) == NULL);
    puts("NATIVE_ACTOR_READY_QUEUE_PASS actors=5 messages=1000 fifo=1 same_actor_max=1 distinct_actor_overlap=2 rejected=3 high_slot=4096");
    return 0;
}
