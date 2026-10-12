#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

enum { ACTORS = 2 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef refs[ACTORS];
    atomic_int active[ACTORS];
    atomic_int peak[ACTORS];
    atomic_int errors;
    atomic_int release_first;
    atomic_int first_started;
    atomic_int own_control;
    atomic_int other_control;
    atomic_int other_during_first;
} State;

static void require(int condition) { if (!condition) abort(); }

static void pause_briefly(void) {
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}

static void enter(State* s, size_t actor) {
    if (!ores_native_actor_scheduler_is_current_carrier(s->scheduler)
            || !ores_native_actor_scheduler_is_current_actor(s->scheduler, s->refs[actor])
            || ores_native_actor_scheduler_is_current_actor(
                s->scheduler, s->refs[1 - actor])) {
        atomic_fetch_add(&s->errors, 1);
    }
    int n = atomic_fetch_add(&s->active[actor], 1) + 1;
    int prior = atomic_load(&s->peak[actor]);
    while (n > prior
            && !atomic_compare_exchange_weak(&s->peak[actor], &prior, n)) { }
    if (n > 1) atomic_fetch_add(&s->errors, 1);
}

static void leave(State* s, size_t actor) {
    if (atomic_fetch_sub(&s->active[actor], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}

static void on_data(void* context, size_t actor, uint64_t payload) {
    State* s = (State*)context;
    if (actor != 0 || payload != 42) abort();
    enter(s, actor);
    atomic_store(&s->first_started, 1);
    while (!atomic_load(&s->release_first)) pause_briefly();
    leave(s, actor);
}

static void on_control(void* context, size_t actor, uint64_t payload) {
    State* s = (State*)context;
    if (payload != 77) abort();
    enter(s, actor);
    if (actor == 0) {
        atomic_fetch_add(&s->own_control, 1);
    } else {
        atomic_fetch_add(&s->other_control, 1);
        if (atomic_load(&s->active[0]) == 1)
            atomic_store(&s->other_during_first, 1);
    }
    leave(s, actor);
}

static void await_flag(atomic_int* flag) {
    /* A bounded external host wait, never run on the actor carrier. */
    for (int n = 0; n < 5000; ++n) {
        if (atomic_load(flag)) return;
        pause_briefly();
    }
    abort();
}

int main(void) {
    State s = {0};
    s.scheduler = ores_native_actor_scheduler_open_control(
        ACTORS, 4, 3, on_data, on_control, &s);
    require(s.scheduler != NULL);
    for (size_t a = 0; a < ACTORS; ++a) {
        s.refs[a] = ores_native_actor_scheduler_spawn(s.scheduler, a);
        require(s.refs[a].scheduler_domain != 0);
    }
    require(!ores_native_actor_scheduler_is_current_carrier(s.scheduler));
    require(!ores_native_actor_scheduler_is_current_actor(s.scheduler, s.refs[0]));
    require(ores_native_actor_scheduler_send(s.scheduler, s.refs[0], 42));
    await_flag(&s.first_started);
    // An external producer, even while this actor is executing elsewhere,
    // must not inherit the actor's thread-local execution authority.
    require(!ores_native_actor_scheduler_is_current_carrier(s.scheduler));
    require(!ores_native_actor_scheduler_is_current_actor(s.scheduler, s.refs[0]));
    require(!ores_native_actor_scheduler_is_current_actor(s.scheduler, s.refs[1]));

    // CONTROL for the same actor must wait; another actor's CONTROL can run.
    require(ores_native_actor_scheduler_publish_control(s.scheduler, s.refs[0], 77));
    require(ores_native_actor_scheduler_publish_control(s.scheduler, s.refs[1], 77));
    await_flag(&s.other_control);
    require(atomic_load(&s.own_control) == 0);
    require(atomic_load(&s.other_during_first) == 1);
    require(atomic_load(&s.active[0]) == 1);

    atomic_store(&s.release_first, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));
    require(atomic_load(&s.own_control) == 1);
    require(atomic_load(&s.other_control) == 1);
    require(atomic_load(&s.peak[0]) == 1);
    require(atomic_load(&s.peak[1]) == 1);
    require(atomic_load(&s.errors) == 0);
    require(!ores_native_actor_scheduler_is_current_actor(s.scheduler, s.refs[0]));
    require(!ores_native_actor_scheduler_is_current_actor(s.scheduler, s.refs[1]));

    for (size_t a = 0; a < ACTORS; ++a)
        require(ores_native_actor_scheduler_stop(s.scheduler, s.refs[a]));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
    return 0;
}
