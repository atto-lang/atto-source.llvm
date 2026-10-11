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
/* When all carriers are busy, CONTROL work must outrank normal work
 * that has not physically started. No speculative normal dispatch tickets. */
typedef struct {
    atomic_int entered, release, delivered;
    int order[3]; /* written by the only carrier thread */
} PriorityState;
static void priority_normal(void* raw, size_t actor, uint64_t payload) {
    PriorityState* p = (PriorityState*)raw;
    check((actor == 0 && payload == 10) || (actor == 1 && payload == 20));
    int slot = atomic_fetch_add(&p->delivered, 1);
    check(slot < 3);
    p->order[slot] = (actor == 0 ? 1 : 2);
    if (actor == 0) {
        atomic_store(&p->entered, 1);
        while (!atomic_load(&p->release)) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
}
static void priority_control(void* raw, size_t actor, uint64_t payload) {
    PriorityState* p = (PriorityState*)raw;
    check(actor == 2 && payload == 30);
    int slot = atomic_fetch_add(&p->delivered, 1);
    check(slot < 3);
    p->order[slot] = 3;
}
int main(void) {
    /* Even a configured scheduler with actors is entirely thread-free before
     * its first accepted runnable item. Retirement of a dormant scheduler
     * joins zero workers and never needs a collector/sweeper thread. */
    State dormant_state = {0};
    OresNativeActorScheduler* dormant =
        ores_native_actor_scheduler_open(2, 4, 8, run, &dormant_state);
    check(dormant != NULL);
    /* Even a legacy request for eight workers must obey the five-worker cap. */
    check(ores_native_actor_scheduler_started_workers(dormant) == 0);
    check(ores_native_actor_scheduler_started_control_threads(dormant) == 0);
    OresNativeActorRef idle = ores_native_actor_scheduler_spawn(dormant, 0);
    check(idle.scheduler_domain != 0);
    check(ores_native_actor_scheduler_started_control_threads(dormant) == 1);
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
    check(ores_native_actor_scheduler_started_control_threads(expanded) == 1);
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
    check(ores_native_actor_scheduler_started_control_threads(scheduler) == 1);
    for (int n = 0; n < EACH; ++n)
        for (size_t a = 0; a < ACTORS; ++a)
            check(ores_native_actor_scheduler_send(scheduler, refs[a], (uint64_t)n));
    check(ores_native_actor_scheduler_started_workers(scheduler) >= 1);
    check(ores_native_actor_scheduler_started_workers(scheduler) <= 5);
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
    /* One busy carrier, one ready normal actor, then a supervisor CONTROL
     * event. CONTROL must be the very next issued turn after the busy actor. */
    PriorityState priority = {0};
    OresNativeActorScheduler* ordered =
        ores_native_actor_scheduler_open_control(
            3, 2, 1, priority_normal, priority_control, &priority);
    check(ordered != NULL);
    OresNativeActorRef pa = ores_native_actor_scheduler_spawn(ordered, 0);
    OresNativeActorRef pb = ores_native_actor_scheduler_spawn(ordered, 1);
    OresNativeActorRef pc = ores_native_actor_scheduler_spawn(ordered, 2);
    check(pa.scheduler_domain && pb.scheduler_domain && pc.scheduler_domain);
    check(ores_native_actor_scheduler_send(ordered, pa, 10));
    int saw_first = 0;
    for (int retry = 0; retry < 3000; ++retry) {
        if (atomic_load(&priority.entered)) { saw_first = 1; break; }
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, NULL);
    }
    check(saw_first);
    check(ores_native_actor_scheduler_send(ordered, pb, 20));
    check(ores_native_actor_scheduler_publish_control(ordered, pc, 30));
    atomic_store(&priority.release, 1);
    check(ores_native_actor_scheduler_drain(ordered));
    check(atomic_load(&priority.delivered) == 3);
    check(priority.order[0] == 1);
    check(priority.order[1] == 3);
    check(priority.order[2] == 2);
    ores_native_actor_scheduler_retire(ordered);
    check(ores_native_actor_scheduler_try_destroy(ordered));
    puts("NATIVE_ACTOR_READY_QUEUE_PASS actors=5 messages=1000 fifo=1 max_active=1 rejected=3");
    return 0;
}
