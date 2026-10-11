#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#define ACTORS 5
#define EACH 200
static void check(int ok) { if (!ok) abort(); }
typedef struct {
    atomic_int active[ACTORS], peak[ACTORS], next[ACTORS], total, errors;
    atomic_int hold;
} State;
static void run(void* raw, size_t actor, uint64_t msg) {
    State* s = (State*)raw;
    int current = atomic_fetch_add(&s->active[actor], 1) + 1;
    int previous = atomic_load(&s->peak[actor]);
    while (current > previous &&
           !atomic_compare_exchange_weak(&s->peak[actor], &previous, current)) { }
    if (current != 1 || (int)msg != atomic_fetch_add(&s->next[actor], 1))
        atomic_fetch_add(&s->errors, 1);
    while (atomic_load(&s->hold)) {
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    atomic_fetch_sub(&s->active[actor], 1);
    atomic_fetch_add(&s->total, 1);
}
int main(void) {
    /* Even a configured scheduler with actors is entirely thread-free before
     * its first accepted runnable item. Retirement of a dormant scheduler
     * joins zero workers and never needs a collector/sweeper thread. */
    State dormant_state = {0};
    OresNativeActorScheduler* dormant =
        ores_native_actor_scheduler_open(2, 4, 8, run, &dormant_state);
    check(dormant != NULL);
    check(ores_native_actor_scheduler_started_workers(dormant) == 0);
    OresNativeActorRef idle = ores_native_actor_scheduler_spawn(dormant, 0);
    check(idle.scheduler_domain != 0);
    check(ores_native_actor_scheduler_started_workers(dormant) == 0);
    ores_native_actor_scheduler_retire(dormant);
    check(ores_native_actor_scheduler_drain(dormant));
    check(ores_native_actor_scheduler_try_destroy(dormant));
    /* First admission starts one worker; sustained concurrent outstanding
     * turns expand to the configured maximum without a GC/helper thread. */
    State growing = {0};
    atomic_store(&growing.hold, 1);
    OresNativeActorScheduler* expanded =
        ores_native_actor_scheduler_open(4, 4, 4, run, &growing);
    check(expanded != NULL);
    OresNativeActorRef growing_refs[4];
    for (size_t i = 0; i < 4; ++i) {
        growing_refs[i] = ores_native_actor_scheduler_spawn(expanded, i);
        check(growing_refs[i].scheduler_domain != 0);
    }
    check(ores_native_actor_scheduler_started_workers(expanded) == 0);
    check(ores_native_actor_scheduler_send(expanded, growing_refs[0], 0));
    check(ores_native_actor_scheduler_started_workers(expanded) == 1);
    for (size_t i = 1; i < 4; ++i)
        check(ores_native_actor_scheduler_send(expanded, growing_refs[i], 0));
    check(ores_native_actor_scheduler_started_workers(expanded) == 4);
    atomic_store(&growing.hold, 0);
    check(ores_native_actor_scheduler_drain(expanded));
    check(atomic_load(&growing.total) == 4);
    ores_native_actor_scheduler_retire(expanded);
    check(ores_native_actor_scheduler_try_destroy(expanded));

    State s = {0};
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(ACTORS, EACH, 8, run, &s);
    check(scheduler != NULL);
    check(ores_native_actor_scheduler_started_workers(scheduler) == 0);
    OresNativeActorRef refs[ACTORS];
    for (int a = 0; a < ACTORS; ++a) {
        refs[a] = ores_native_actor_scheduler_spawn(scheduler, (size_t)a);
        check(refs[a].scheduler_domain && ores_native_actor_scheduler_poll_ready(scheduler, refs[a]));
        check(!ores_native_actor_scheduler_spawn(scheduler, (size_t)a).scheduler_domain);
    }
    check(ores_native_actor_scheduler_started_workers(scheduler) == 0);
    for (int n = 0; n < EACH; ++n)
        for (size_t a = 0; a < ACTORS; ++a)
            check(ores_native_actor_scheduler_send(scheduler, refs[a], (uint64_t)n));
    check(ores_native_actor_scheduler_started_workers(scheduler) >= 1);
    check(ores_native_actor_scheduler_started_workers(scheduler) <= 8);
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
    }
    ores_native_actor_scheduler_retire(scheduler);
    check(!ores_native_actor_scheduler_submit(scheduler, 0, 999));
    check(ores_native_actor_scheduler_try_destroy(scheduler));

    State pressure = {0};
    atomic_store(&pressure.hold, 1);
    scheduler = ores_native_actor_scheduler_open(1, 1, 2, run, &pressure);
    check(scheduler != NULL);
    check(ores_native_actor_scheduler_started_workers(scheduler) == 0);
    OresNativeActorRef one = ores_native_actor_scheduler_spawn(scheduler, 0);
    check(one.scheduler_domain != 0);
    check(ores_native_actor_scheduler_try_send(scheduler, refs[0], 0)
          == ORES_NATIVE_SEND_INVALID_REF); /* foreign domain */
    OresNativeActorRef stale = one;
    stale.generation++;
    check(ores_native_actor_scheduler_try_send(scheduler, stale, 0)
          == ORES_NATIVE_SEND_INVALID_REF);
    check(ores_native_actor_scheduler_started_workers(scheduler) == 0);
    check(ores_native_actor_scheduler_try_send(scheduler, one, 0)
          == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_actor_scheduler_started_workers(scheduler) == 1);
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
    ores_native_actor_scheduler_retire(scheduler);
    check(!ores_native_actor_scheduler_try_destroy(scheduler));
    atomic_store(&pressure.hold, 0);
    check(ores_native_actor_scheduler_drain(scheduler));
    check(ores_native_actor_scheduler_completed(scheduler) == 1);
    check(ores_native_actor_scheduler_poll_done(scheduler, one));
    check(ores_native_actor_scheduler_try_destroy(scheduler));
    puts("NATIVE_ACTOR_READY_QUEUE_PASS actors=5 messages=1000 fifo=1 max_active=1 rejected=3");
    return 0;
}
