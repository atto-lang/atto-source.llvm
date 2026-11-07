#define _POSIX_C_SOURCE 200809L
// Host-only exact carrier admission and STOP/commit linearization test.
// This uses a preexisting scalar host counter, not a second ActorLocal store
// and NOT a compiler-produced .atto guest.
#include "oreslang/runtime/NativeActorScheduler.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actors[2];
    atomic_uint_fast64_t commits[2];
    atomic_int completed[2];
    atomic_int stopped_actor_entered;
    atomic_int release_stopped_actor;
} Scenario;

static int bounded_atomic_increment(void* opaque) {
    // A single lock-free scalar operation: no callbacks, locks, allocation,
    // scheduler API, external resource accesses or destructor invocation.
    atomic_fetch_add_explicit((atomic_uint_fast64_t*)opaque, 1,
                              memory_order_relaxed);
    return 1;
}

static void until(atomic_int* flag, int threshold) {
    struct timespec start, now;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        if (atomic_load_explicit(flag, memory_order_acquire) >= threshold)
            return;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        const int64_t elapsed_ns =
            (int64_t)(now.tv_sec - start.tv_sec) * INT64_C(1000000000) +
            (int64_t)now.tv_nsec - (int64_t)start.tv_nsec;
        assert(elapsed_ns < INT64_C(8000000000));
        sched_yield();
    }
}

static void on_message(void* context, size_t index, uint64_t payload) {
    Scenario* s = (Scenario*)context;
    assert(index < 2 && (payload == 1 || payload == 2));
    assert(ores_native_actor_scheduler_is_current_actor(
        s->scheduler, s->actors[index]));
    assert(ores_native_actor_scheduler_commit_if_running_v1(
        s->scheduler, s->actors[index],
        bounded_atomic_increment, &s->commits[index]));
    // Another active actor on the same scheduler cannot be used as a
    // commit target by the physical carrier of this actor.
    assert(!ores_native_actor_scheduler_commit_if_running_v1(
        s->scheduler, s->actors[1 - index],
        bounded_atomic_increment, &s->commits[1 - index]));
    if (index == 0 && payload == 2) {
        atomic_store_explicit(&s->stopped_actor_entered, 1,
                              memory_order_release);
        until(&s->release_stopped_actor, 1);
        // The caller still owns its physical callback, but STOP already
        // won the scheduler lifecycle lock: no new writes may commit.
        assert(!ores_native_actor_scheduler_commit_if_running_v1(
            s->scheduler, s->actors[0],
            bounded_atomic_increment, &s->commits[0]));
    }
    atomic_fetch_add_explicit(&s->completed[index], 1,
                              memory_order_release);
}
int main(void) {
    Scenario s = {0};
    atomic_init(&s.commits[0], 0);
    atomic_init(&s.commits[1], 0);
    atomic_init(&s.completed[0], 0);
    atomic_init(&s.completed[1], 0);
    atomic_init(&s.stopped_actor_entered, 0);
    atomic_init(&s.release_stopped_actor, 0);
    // The scheduler callback must never enter a libatomic fallback mutex.
    assert(atomic_is_lock_free(&s.commits[0]));
    assert(atomic_is_lock_free(&s.commits[1]));
    s.scheduler = ores_native_actor_scheduler_open(2, 8, 2, on_message, &s);
    assert(s.scheduler);
    for (size_t i = 0; i < 2; i++) {
        s.actors[i] = ores_native_actor_scheduler_spawn(s.scheduler, i);
        assert(s.actors[i].scheduler_domain);
    }
    // A foreign OS thread has no carrier/actor claim. Do not treat an
    // exact live ActorRef or pointer to a host state cell as authority.
    assert(!ores_native_actor_scheduler_commit_if_running_v1(
        s.scheduler, s.actors[0],
        bounded_atomic_increment, &s.commits[0]));
    assert(ores_native_actor_scheduler_try_send(s.scheduler,s.actors[0],1)
           == ORES_NATIVE_SEND_ACCEPTED);
    assert(ores_native_actor_scheduler_try_send(s.scheduler,s.actors[1],1)
           == ORES_NATIVE_SEND_ACCEPTED);
    until(&s.completed[0], 1);
    until(&s.completed[1], 1);
    assert(atomic_load(&s.commits[0]) == 1);
    assert(atomic_load(&s.commits[1]) == 1);

    assert(ores_native_actor_scheduler_try_send(s.scheduler,s.actors[0],2)
           == ORES_NATIVE_SEND_ACCEPTED);
    until(&s.stopped_actor_entered, 1);
    assert(atomic_load(&s.commits[0]) == 2);
    // The actor remains RUNNING while its callback waits, but this is not
    // the carrier of that actor; legacy gate would accept this forged call.
    assert(!ores_native_actor_scheduler_commit_if_running_v1(
        s.scheduler, s.actors[0],
        bounded_atomic_increment, &s.commits[0]));
    assert(atomic_load(&s.commits[0]) == 2);
    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actors[0]));
    // Even on the same OS thread that stopped the actor, no delayed write.
    assert(!ores_native_actor_scheduler_commit_if_running_v1(
        s.scheduler, s.actors[0],
        bounded_atomic_increment, &s.commits[0]));
    atomic_store_explicit(&s.release_stopped_actor,1,memory_order_release);
    until(&s.completed[0],2);
    assert(atomic_load(&s.commits[0]) == 2);
    assert(ores_native_actor_scheduler_wait_done(s.scheduler,s.actors[0]));
    assert(ores_native_actor_scheduler_try_send(s.scheduler,s.actors[0],3)
           != ORES_NATIVE_SEND_ACCEPTED);

    // A sibling actor retains its independent live scalar commit authority.
    assert(ores_native_actor_scheduler_try_send(s.scheduler,s.actors[1],2)
           == ORES_NATIVE_SEND_ACCEPTED);
    until(&s.completed[1],2);
    assert(atomic_load(&s.commits[1]) == 2);
    assert(ores_native_actor_scheduler_stop(s.scheduler,s.actors[1]));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler,s.actors[1]));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
    puts("ACTOR_BOUND_COMMIT_CARRIER_STOP_RACE_HOST_PASS");
    return 0;
}
