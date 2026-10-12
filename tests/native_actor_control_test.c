#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
typedef struct {
    OresNativeActorScheduler* group;
    OresNativeActorRef parent, child;
    atomic_int normal_active, started, release, child_posted;
    atomic_int order, wrong_lane, control_count, failed_post_count;
    pthread_t child_carrier;
} State;
static void check(int condition) { if (!condition) abort(); }
static void pause_short(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static void wait_until(atomic_int* p, int expect) {
    for (int i = 0; i < 3000; ++i) {
        if (atomic_load(p) >= expect) return;
        pause_short();
    }
    check(0);
}
static void normal(void* arg, size_t actor, uint64_t payload) {
    State* state = (State*)arg;
    if (actor == 0 && payload == 7) {
        check(atomic_fetch_add(&state->order, 1) == 0);
        atomic_store(&state->normal_active, 1);
        atomic_store(&state->started, 1);
        while (!atomic_load(&state->release)) pause_short();
        atomic_store(&state->normal_active, 0);
    } else if (actor == 0 && payload == 9) {
        /* Supervisor CONTROL must run before this already-queued normal. */
        if (atomic_fetch_add(&state->order, 1) != 2)
            atomic_fetch_add(&state->wrong_lane, 1);
    } else if (actor == 1 && payload == 8) {
        state->child_carrier = pthread_self();
        check(ores_native_actor_scheduler_post_control(
            state->group, state->parent, state->child,
            ORES_ACTOR_CONTROL_PANIC, 1234) == 1);
        /* The parent's single preallocated CONTROL slot is occupied.
         * Caller retains an unaccepted event for retry/escalation. */
        check(ores_native_actor_scheduler_post_control(
            state->group, state->parent, state->child,
            ORES_ACTOR_CONTROL_CANCELLED, 5678) == 0);
        atomic_fetch_add(&state->failed_post_count, 1);
        atomic_store(&state->child_posted, 1);
    } else abort();
}
static void control(void* arg, size_t parent, OresNativeActorControlKind kind,
                    OresNativeActorRef child, uint64_t payload) {
    State* s = (State*)arg;
    if (parent != 0 || kind != ORES_ACTOR_CONTROL_PANIC || payload != 1234
        || child.scheduler_domain != s->child.scheduler_domain
        || child.actor_slot != s->child.actor_slot
        || child.generation != s->child.generation
        || atomic_load(&s->normal_active)
        || pthread_equal(pthread_self(), s->child_carrier)
        || atomic_fetch_add(&s->order, 1) != 1)
        atomic_fetch_add(&s->wrong_lane, 1);
    atomic_fetch_add(&s->control_count, 1);
}
int main(void) {
    State s = {0};
    s.group = ores_native_actor_scheduler_open(2, 2, 3, normal, &s);
    check(s.group != NULL);
    check(ores_native_actor_scheduler_set_control_handler(s.group, control));
    check(!ores_native_actor_scheduler_set_control_handler(s.group, control));
    s.parent = ores_native_actor_scheduler_spawn(s.group, 0);
    s.child = ores_native_actor_scheduler_spawn(s.group, 1);
    check(s.parent.scheduler_domain && s.child.scheduler_domain);
    check(ores_native_actor_scheduler_send(s.group, s.parent, 7));
    wait_until(&s.started, 1);
    check(ores_native_actor_scheduler_send(s.group, s.parent, 9));
    /* Parent mailbox is full: 1 active + 1 normal queued. */
    check(!ores_native_actor_scheduler_send(s.group, s.parent, 17));
    check(ores_native_actor_scheduler_send(s.group, s.child, 8));
    wait_until(&s.child_posted, 1);
    check(ores_native_actor_scheduler_control_accepted(s.group) == 1);
    check(ores_native_actor_scheduler_control_completed(s.group) == 0);
    check(ores_native_actor_scheduler_accepted(s.group) == 4);
    /* No callback from the child carrier and no bypass of parent exclusivity. */
    check(atomic_load(&s.control_count) == 0);
    atomic_store(&s.release, 1);
    check(ores_native_actor_scheduler_drain(s.group));
    check(atomic_load(&s.order) == 3);
    check(atomic_load(&s.wrong_lane) == 0);
    check(atomic_load(&s.control_count) == 1);
    check(atomic_load(&s.failed_post_count) == 1);
    check(ores_native_actor_scheduler_control_completed(s.group) == 1);
    check(ores_native_actor_scheduler_stop(s.group, s.child));
    check(ores_native_actor_scheduler_stop(s.group, s.parent));
    check(ores_native_actor_scheduler_poll_done(s.group, s.parent));
    check(!ores_native_actor_scheduler_post_control(
        s.group, s.parent, s.child, ORES_ACTOR_CONTROL_DONE, 0));
    ores_native_actor_scheduler_retire(s.group);
    check(ores_native_actor_scheduler_try_destroy(s.group));
    puts("NATIVE_SUPERVISOR_CONTROL_PASS reserved_slot=1 parent_exclusive=1 panic_typed=1");
    return 0;
}
