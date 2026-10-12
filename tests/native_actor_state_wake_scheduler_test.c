#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent, child;
    atomic_int started, release, in_actor, errors, count;
    int seen[4];
    OresNativeActorStateWake wake;
} State;

static void check(int okay) { if (!okay) abort(); }
static void tick(void) {
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}
static void until(atomic_int* flag) {
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load(flag)) return;
        tick();
    }
    abort();
}
static void enter(State* s, size_t actor, int event) {
    if (actor != 0
        || !ores_native_actor_scheduler_is_current_actor(s->scheduler, s->parent)
        || ores_native_actor_scheduler_is_current_actor(s->scheduler, s->child)
        || atomic_fetch_add(&s->in_actor, 1) != 0) {
        atomic_fetch_add(&s->errors, 1);
    }
    int offset = atomic_fetch_add(&s->count, 1);
    if (offset >= 4) abort();
    s->seen[offset] = event;
}
static void leave(State* s) {
    if (atomic_fetch_sub(&s->in_actor, 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void on_data(void* arg, size_t actor, uint64_t value) {
    State* s = (State*)arg;
    if (value == 101) {
        enter(s, actor, 1);
        atomic_store(&s->started, 1);
        while (!atomic_load(&s->release)) tick();
    } else {
        check(value == 102);
        enter(s, actor, 4);
    }
    leave(s);
}
static void on_control(void* arg, size_t actor, uint64_t payload) {
    (void)arg; (void)actor; (void)payload;
    abort();
}
static void on_panic(void* arg, size_t actor, OresNativeActorTerminalFailure f) {
    State* s = (State*)arg;
    check(f.kind == ORES_NATIVE_CONTROL_PANIC && f.failure_code == 451);
    check(f.child.scheduler_domain == s->child.scheduler_domain);
    enter(s, actor, 2);
    leave(s);
}
static void on_state(void* arg, size_t actor, OresNativeActorStateWake wake) {
    State* s = (State*)arg;
    enter(s, actor, 3);
    check(wake.incarnation == s->wake.incarnation);
    check(wake.continuation_id == s->wake.continuation_id);
    check(wake.status == s->wake.status);
    leave(s);
}
int main(void) {
    State s = {0};
    s.scheduler = ores_native_actor_scheduler_open_state_wakes(
        2, 4, 3, on_data, on_control, on_panic, on_state, &s);
    check(s.scheduler != NULL);
    s.parent = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    s.child = ores_native_actor_scheduler_spawn(s.scheduler, 1);
    check(s.parent.scheduler_domain && s.child.scheduler_domain);
    check(ores_native_actor_scheduler_send(s.scheduler, s.parent, 101));
    until(&s.started);
    check(ores_native_actor_scheduler_send(s.scheduler, s.parent, 102));
    s.wake = (OresNativeActorStateWake){1234, 789, ORES_NATIVE_RW_READY};
    OresNativeActorRef stale = s.parent;
    ++stale.generation;
    check(!ores_native_actor_scheduler_publish_state_wake(
        s.scheduler, stale, s.wake));
    OresNativeActorStateWake forged = s.wake;
    forged.status = 999;
    check(!ores_native_actor_scheduler_publish_state_wake(
        s.scheduler, s.parent, forged));
    check(ores_native_actor_scheduler_publish_state_wake(
        s.scheduler, s.parent, s.wake));
    check(!ores_native_actor_scheduler_publish_state_wake(
        s.scheduler, s.parent, s.wake)); // bounded, unconsumed
    OresNativeActorTerminalFailure failure = {
        ORES_NATIVE_CONTROL_PANIC, s.child, 451, 12
    };
    // RW wake cannot take the supervisor's reserved priority slot.
    check(ores_native_actor_scheduler_publish_failure(
        s.scheduler, s.parent, failure));
    check(!ores_native_actor_scheduler_publish_failure(
        s.scheduler, s.parent, failure));
    check(atomic_load(&s.count) == 1);
    atomic_store(&s.release, 1);
    check(ores_native_actor_scheduler_drain(s.scheduler));
    check(atomic_load(&s.count) == 4);
    check(s.seen[0] == 1 && s.seen[1] == 2
          && s.seen[2] == 3 && s.seen[3] == 4);
    check(atomic_load(&s.in_actor) == 0 && atomic_load(&s.errors) == 0);
    check(ores_native_actor_scheduler_state_wakes_completed(s.scheduler) == 1);
    check(ores_native_actor_scheduler_control_completed(s.scheduler) == 1);
    check(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    check(ores_native_actor_scheduler_stop(s.scheduler, s.child));
    check(!ores_native_actor_scheduler_publish_state_wake(
        s.scheduler, s.parent, s.wake));
    ores_native_actor_scheduler_retire(s.scheduler);
    check(ores_native_actor_scheduler_try_destroy(s.scheduler));
    return 0;
}
