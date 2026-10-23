#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { CHILDREN = 1000, ACTORS = CHILDREN + 1 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef parent;
    atomic_int entered, release, concurrent, peak;
    unsigned char seen[CHILDREN];
    size_t count;
} State;

static void require(int ok) { if (!ok) abort(); }

static void pause_short(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}

static void enter(State* s, size_t actor) {
    require(actor == 0);
    require(ores_native_actor_scheduler_is_current_actor(s->scheduler, s->parent));
    int count = atomic_fetch_add(&s->concurrent, 1) + 1;
    if (count > atomic_load(&s->peak)) atomic_store(&s->peak, count);
    require(count == 1);
}

static void leave(State* s) {
    require(atomic_fetch_sub(&s->concurrent, 1) == 1);
}

static void on_data(void* opaque, size_t actor, uint64_t payload) {
    State* s = (State*)opaque;
    require(payload == 7);
    enter(s, actor);
    atomic_store(&s->entered, 1);
    while (!atomic_load(&s->release)) pause_short();
    leave(s);
}
static void on_control(void* opaque, size_t actor, uint64_t payload) {
    (void)opaque; (void)actor; (void)payload;
    abort();
}
static void on_failure(void* opaque, size_t actor,
                       OresNativeActorTerminalFailure event) {
    State* s = (State*)opaque;
    enter(s, actor);
    require(event.kind == ORES_NATIVE_CONTROL_PANIC);
    require(event.child.scheduler_domain == s->parent.scheduler_domain);
    require(event.child.actor_slot >= 1 && event.child.actor_slot <= CHILDREN);
    require(event.child.generation != 0);
    size_t n = (size_t)(event.child.actor_slot - 1);
    require(event.correlation == (uint64_t)(n + 1));
    require(event.failure_code == 99);
    require(s->count == n); /* Intrusive per-parent admission FIFO. */
    require(!s->seen[n]);
    s->seen[n] = 1;
    ++s->count;
    leave(s);
}

int main(void) {
    State s = {0};
    s.scheduler = ores_native_actor_scheduler_open_typed_control(
        ACTORS, 1, 2, on_data, on_control, on_failure, &s);
    require(s.scheduler != NULL);
    s.parent = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    require(s.parent.scheduler_domain != 0);
    OresNativeActorRef children[CHILDREN];
    for (size_t i = 0; i < CHILDREN; ++i) {
        children[i] = ores_native_actor_scheduler_spawn(s.scheduler, i + 1);
        require(children[i].generation != 0);
        require(ores_native_actor_scheduler_bind_supervisor(
            s.scheduler, s.parent, children[i]));
        require(!ores_native_actor_scheduler_bind_supervisor(
            s.scheduler, s.parent, children[i]));
    }

    require(ores_native_actor_scheduler_send(s.scheduler, s.parent, 7));
    for (int i = 0; !atomic_load(&s.entered) && i < 5000; ++i) pause_short();
    require(atomic_load(&s.entered));
    require(ores_native_actor_scheduler_try_send(s.scheduler, s.parent, 8)
            == ORES_NATIVE_SEND_MAILBOX_FULL);

    OresNativeActorTerminalFailure forged = {
        ORES_NATIVE_CONTROL_PANIC, children[0], 99, 1
    };
    ++forged.child.generation;
    require(!ores_native_actor_scheduler_publish_failure(
        s.scheduler, s.parent, forged));
    require(!ores_native_actor_scheduler_bind_supervisor(
        s.scheduler, children[0], children[0]));

    for (size_t i = 0; i < CHILDREN; ++i) {
        OresNativeActorTerminalFailure event = {
            ORES_NATIVE_CONTROL_PANIC, children[i], 99, i + 1
        };
        require(ores_native_actor_scheduler_publish_failure(
            s.scheduler, s.parent, event));
        /* Duplicate publication must never mint another receipt or change
         * the first event's failure data. */
        event.failure_code = 100;
        require(!ores_native_actor_scheduler_publish_failure(
            s.scheduler, s.parent, event));
    }
    require(s.count == 0);
    atomic_store(&s.release, 1);
    require(ores_native_actor_scheduler_drain(s.scheduler));
    require(s.count == CHILDREN);
    require(atomic_load(&s.peak) == 1);
    require(ores_native_actor_scheduler_control_completed(s.scheduler) == CHILDREN);
    for (size_t i = 0; i < CHILDREN; ++i) {
        require(s.seen[i] == 1);
        require(ores_native_actor_scheduler_stop(s.scheduler, children[i]));
    }
    require(ores_native_actor_scheduler_stop(s.scheduler, s.parent));
    require(ores_native_actor_scheduler_wait_done(s.scheduler, s.parent));
    ores_native_actor_scheduler_retire(s.scheduler);
    require(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("NATIVE_SUPERVISOR_1000_FAILURES_PASS bound=1000 delivered=1000");
    return 0;
}
