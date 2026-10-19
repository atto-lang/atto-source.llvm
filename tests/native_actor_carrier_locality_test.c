#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

enum { ACTORS = 8, ROUNDS = 40 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef refs[ACTORS];
    atomic_int active[ACTORS];
    atomic_int next[ACTORS];
    atomic_int errors;
    atomic_int gate_enabled, gate_entered, gate_release;
    atomic_int hot_executed, cold_after_hot;
} State;

static void on_message(void* opaque, size_t actor, uint64_t message) {
    State* s = (State*)opaque;
    int slot = ores_native_actor_scheduler_current_carrier_slot();
    int cpuTarget = ores_native_actor_scheduler_current_cpu_target();
    if (cpuTarget < -1) atomic_fetch_add(&s->errors, 1);
    if (slot < 0 || slot >= 4 || actor >= ACTORS
        || !ores_native_actor_scheduler_is_current_actor(s->scheduler, s->refs[actor]))
        atomic_fetch_add(&s->errors, 1);
    if (atomic_fetch_add(&s->active[actor], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
    if (atomic_fetch_add(&s->next[actor], 1) != (int)message)
        atomic_fetch_add(&s->errors, 1);
    if (atomic_load(&s->gate_enabled) && actor == 0 && message == 0) {
        atomic_store(&s->gate_entered, 1);
        while (!atomic_load(&s->gate_release)) {
            struct timespec pause = {0, 1000000};
            nanosleep(&pause, NULL);
        }
    }
    if (atomic_load(&s->gate_enabled)) {
        if (actor == 0) atomic_fetch_add(&s->hot_executed, 1);
        if (actor == 1)
            atomic_store(&s->cold_after_hot, atomic_load(&s->hot_executed));
    }
    if (atomic_fetch_sub(&s->active[actor], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void close_scheduler(State* s, size_t count) {
    assert(ores_native_actor_scheduler_drain(s->scheduler));
    for (size_t i = 0; i < count; i++)
        assert(ores_native_actor_scheduler_stop(s->scheduler, s->refs[i]));
    ores_native_actor_scheduler_retire(s->scheduler);
    assert(ores_native_actor_scheduler_try_destroy(s->scheduler));
}
int main(void) {
    State one = {0};
    assert(ores_native_actor_scheduler_current_carrier_slot() == -1);
    one.scheduler = ores_native_actor_scheduler_open(1, 1, 1, on_message, &one);
    assert(one.scheduler);
    one.refs[0] = ores_native_actor_scheduler_spawn(one.scheduler, 0);
    assert(one.refs[0].scheduler_domain);
    for (int i = 0; i < 96; i++) {
        assert(ores_native_actor_scheduler_send(one.scheduler, one.refs[0], (uint64_t)i));
        assert(ores_native_actor_scheduler_drain(one.scheduler));
    }
    assert(atomic_load(&one.next[0]) == 96);
    assert(atomic_load(&one.errors) == 0);
    assert(ores_native_actor_scheduler_preferred_carrier_hits(one.scheduler) == 95);
    assert(ores_native_actor_scheduler_carrier_migrations(one.scheduler) == 0);
    close_scheduler(&one, 1);
    assert(ores_native_actor_scheduler_current_carrier_slot() == -1);

    State multiple = {0};
    multiple.scheduler = ores_native_actor_scheduler_open(
        ACTORS, ROUNDS, 4, on_message, &multiple);
    assert(multiple.scheduler);
    for (size_t actor = 0; actor < ACTORS; actor++) {
        multiple.refs[actor] = ores_native_actor_scheduler_spawn(multiple.scheduler, actor);
        assert(multiple.refs[actor].scheduler_domain);
    }
    for (int i = 0; i < ROUNDS; i++)
        for (size_t a = 0; a < ACTORS; a++)
            assert(ores_native_actor_scheduler_send(
                multiple.scheduler, multiple.refs[a], (uint64_t)i));
    assert(ores_native_actor_scheduler_drain(multiple.scheduler));
    assert(atomic_load(&multiple.errors) == 0);
    for (int a = 0; a < ACTORS; a++)
        assert(atomic_load(&multiple.next[a]) == ROUNDS);
    assert(ores_native_actor_scheduler_preferred_carrier_hits(multiple.scheduler)
           + ores_native_actor_scheduler_carrier_migrations(multiple.scheduler)
           <= ACTORS * ROUNDS);
    close_scheduler(&multiple, ACTORS);

    /* A constantly runnable familiar actor must not monopolize the only
     * carrier indefinitely while a new actor lacks an affinity history. */
    State fair = {0};
    atomic_store(&fair.gate_enabled, 1);
    atomic_store(&fair.cold_after_hot, -1);
    fair.scheduler = ores_native_actor_scheduler_open(2, 64, 1, on_message, &fair);
    assert(fair.scheduler);
    for (size_t a = 0; a < 2; a++) {
        fair.refs[a] = ores_native_actor_scheduler_spawn(fair.scheduler, a);
        assert(fair.refs[a].scheduler_domain);
    }
    assert(ores_native_actor_scheduler_send(fair.scheduler, fair.refs[0], 0));
    int started = 0;
    for (int i = 0; i < 5000; i++) {
        if (atomic_load(&fair.gate_entered)) { started = 1; break; }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, NULL);
    }
    assert(started);
    for (int i = 1; i < 32; i++)
        assert(ores_native_actor_scheduler_send(fair.scheduler, fair.refs[0], (uint64_t)i));
    assert(ores_native_actor_scheduler_send(fair.scheduler, fair.refs[1], 0));
    atomic_store(&fair.gate_release, 1);
    assert(ores_native_actor_scheduler_drain(fair.scheduler));
    assert(atomic_load(&fair.next[0]) == 32);
    assert(atomic_load(&fair.next[1]) == 1);
    assert(atomic_load(&fair.errors) == 0);
    assert(atomic_load(&fair.cold_after_hot) >= 1);
    assert(atomic_load(&fair.cold_after_hot) <= 9);
    close_scheduler(&fair, 2);
    puts("NATIVE_CARRIER_LOCALITY_PASS one_worker_reuse=95 multiple_actors=8 fairness_burst=8");
    return 0;
}
