#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include "test_barrier.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { ACTORS = 4096, SENDERS = 4, ROUNDS = 12 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef refs[ACTORS];
    OresTestBarrier gate;
    atomic_uint first_seen[ACTORS];
    atomic_uint second_seen[ACTORS];
    atomic_uint control_seen[ACTORS];
    atomic_uint second_admitted[ACTORS];
    atomic_uint control_admitted[ACTORS];
    atomic_uint active[ACTORS];
    atomic_uint errors;
    unsigned salt;
} State;
typedef struct { State* state; unsigned sender; } Producer;

static size_t slot_at(unsigned order, unsigned salt) {
    return (size_t)(((uint64_t)order * UINT64_C(3359) + salt) % ACTORS);
}
static void on_data(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    if (actor >= ACTORS) abort();
    if (!ores_native_actor_scheduler_is_current_actor(s->scheduler, s->refs[actor])
        || atomic_fetch_add(&s->active[actor], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
    if (payload == (uint64_t)(actor * 2)) {
        if (atomic_fetch_add(&s->first_seen[actor], 1) != 0)
            atomic_fetch_add(&s->errors, 1);
    } else if (payload == (uint64_t)(actor * 2 + 1)) {
        if (atomic_load(&s->first_seen[actor]) != 1
            || atomic_fetch_add(&s->second_seen[actor], 1) != 0)
            atomic_fetch_add(&s->errors, 1);
    } else atomic_fetch_add(&s->errors, 1);
    if (atomic_fetch_sub(&s->active[actor], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void on_control(void* raw, size_t actor, uint64_t payload) {
    State* s = (State*)raw;
    if (actor >= ACTORS) abort();
    if (!ores_native_actor_scheduler_is_current_actor(s->scheduler, s->refs[actor])
        || payload != ((uint64_t)actor ^ UINT64_C(0xdadabc12))
        || atomic_fetch_add(&s->active[actor], 1) != 0
        || atomic_fetch_add(&s->control_seen[actor], 1) != 0)
        atomic_fetch_add(&s->errors, 1);
    sched_yield();
    if (atomic_fetch_sub(&s->active[actor], 1) != 1)
        atomic_fetch_add(&s->errors, 1);
}
static void* send_worker(void* raw) {
    Producer* producer = (Producer*)raw;
    State* s = producer->state;
    ores_test_barrier_wait(&s->gate);
    for (unsigned i = producer->sender; i < ACTORS; i += SENDERS) {
        size_t actor = slot_at(i, s->salt);
        if (((i + s->salt) & 7u) == 0u) sched_yield();
        OresNativeSendStatus status = ores_native_actor_scheduler_try_send(
            s->scheduler, s->refs[actor], actor * 2 + 1);
        if (status == ORES_NATIVE_SEND_ACCEPTED)
            atomic_store(&s->second_admitted[actor], 1);
        else if (status != ORES_NATIVE_SEND_TERMINATED)
            atomic_fetch_add(&s->errors, 1);
    }
    return NULL;
}
static void* control_worker(void* raw) {
    State* s = (State*)raw;
    ores_test_barrier_wait(&s->gate);
    for (unsigned i = 0; i < ACTORS; ++i) {
        size_t actor = slot_at(i, s->salt ^ 0xbad);
        if ((i & 15u) == 0u) sched_yield();
        if (ores_native_actor_scheduler_publish_control(
                s->scheduler, s->refs[actor],
                (uint64_t)actor ^ UINT64_C(0xdadabc12)))
            atomic_store(&s->control_admitted[actor], 1);
    }
    return NULL;
}
static void* stop_worker(void* raw) {
    State* s = (State*)raw;
    ores_test_barrier_wait(&s->gate);
    for (unsigned i = 0; i < ACTORS; ++i) {
        size_t actor = slot_at(i, s->salt + 17);
        if (((i + s->salt) & 3u) == 0u) sched_yield();
        if (!ores_native_actor_scheduler_stop(s->scheduler, s->refs[actor]))
            atomic_fetch_add(&s->errors, 1);
        if (ores_native_actor_scheduler_try_send(
                s->scheduler, s->refs[actor], UINT64_MAX)
            != ORES_NATIVE_SEND_TERMINATED)
            atomic_fetch_add(&s->errors, 1);
    }
    return NULL;
}
static void run_round(unsigned salt) {
    State* s = (State*)calloc(1, sizeof(*s));
    assert(s);
    s->salt = salt;
    s->scheduler = ores_native_actor_scheduler_open_control(
        ACTORS, 3, 8, on_data, on_control, s);
    assert(s->scheduler != NULL);
    for (size_t actor = 0; actor < ACTORS; ++actor) {
        s->refs[actor] = ores_native_actor_scheduler_spawn(s->scheduler, actor);
        assert(s->refs[actor].scheduler_domain);
        assert(ores_native_actor_scheduler_try_send(
            s->scheduler, s->refs[actor], actor * 2)
            == ORES_NATIVE_SEND_ACCEPTED);
    }
    assert(ores_test_barrier_init(&s->gate, SENDERS + 2) == 0);
    pthread_t senders[SENDERS], controller, stopper;
    Producer args[SENDERS];
    for (unsigned i = 0; i < SENDERS; ++i) {
        args[i] = (Producer){s, i};
        assert(pthread_create(&senders[i], NULL, send_worker, &args[i]) == 0);
    }
    assert(pthread_create(&controller, NULL, control_worker, s) == 0);
    assert(pthread_create(&stopper, NULL, stop_worker, s) == 0);
    for (unsigned i = 0; i < SENDERS; ++i)
        assert(pthread_join(senders[i], NULL) == 0);
    assert(pthread_join(controller, NULL) == 0);
    assert(pthread_join(stopper, NULL) == 0);
    assert(ores_test_barrier_destroy(&s->gate) == 0);
    ores_native_actor_scheduler_retire(s->scheduler);
    assert(ores_native_actor_scheduler_drain(s->scheduler));
    assert(atomic_load(&s->errors) == 0);
    uint64_t delivered = 0;
    for (size_t actor = 0; actor < ACTORS; ++actor) {
        assert(atomic_load(&s->first_seen[actor]) == 1);
        assert(atomic_load(&s->second_seen[actor])
            == atomic_load(&s->second_admitted[actor]));
        assert(atomic_load(&s->control_seen[actor])
            == atomic_load(&s->control_admitted[actor]));
        assert(atomic_load(&s->active[actor]) == 0);
        assert(ores_native_actor_scheduler_poll_done(s->scheduler, s->refs[actor]));
        delivered += 1 + atomic_load(&s->second_admitted[actor])
            + atomic_load(&s->control_admitted[actor]);
    }
    assert(ores_native_actor_scheduler_accepted(s->scheduler) == delivered);
    assert(ores_native_actor_scheduler_completed(s->scheduler) == delivered);
    assert(ores_native_actor_scheduler_try_destroy(s->scheduler));
    free(s);
}
int main(void) {
    for (unsigned round = 0; round < ROUNDS; ++round)
        run_round((round * 97u) ^ 0x35abu);
    puts("NATIVE_READY_SUMMARY_RACE_PASS rounds=12 actors=4096 senders=4 fifo=1 control=1 retired=1");
    return 0;
}
