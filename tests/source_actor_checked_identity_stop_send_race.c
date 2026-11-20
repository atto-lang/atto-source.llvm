/*
 * Trusted native SourceActor checked-ID producer/stop admission race.
 * Four independent OS producers continuously submit DATA to the same actor
 * until its root caller seals it. Previously admitted DATA may execute or
 * be abandoned; NO DATA admission is permitted after the stop returns.
 *
 * This exercises SourceActorBootstrap and the native scheduler only:
 * not guest task suspension, generic actor RPC or external FFI capabilities.
 */
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { PRODUCERS = 4, GENERATIONS = 12, MIN_ATTEMPTS = 8,
       POST_STOP_REJECTIONS = 128 };
static atomic_uint constructors;
static atomic_uint messages;

static void require(int okay) {
    if (!okay) {
        fputs("SOURCE_ACTOR_CHECKED_ID_STOP_SEND_RACE_FAIL\n", stderr);
        abort();
    }
}
static void initialize(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    require(heap != NULL && domain != 0 && epoch != 0);
    atomic_fetch_add_explicit(&constructors, 1, memory_order_relaxed);
}
static int64_t receive(OresActorI64Heap* heap, uint64_t domain,
                       uint64_t epoch, int64_t value) {
    require(heap != NULL && domain != 0 && epoch != 0);
    atomic_fetch_add_explicit(&messages, 1, memory_order_relaxed);
    return value + 1;
}

typedef struct {
    OresSourceActorId id;
    atomic_int go;
    atomic_int stop_admitted;
    atomic_uint attempts[PRODUCERS];
    atomic_uint accepted[PRODUCERS];
    atomic_uint post_stop_checked[PRODUCERS];
} Race;
typedef struct {
    Race* race;
    unsigned index;
} Producer;

static void* run_producer(void* opaque) {
    Producer* producer = (Producer*)opaque;
    Race* race = producer->race;
    unsigned index = producer->index;
    while (!atomic_load_explicit(&race->go, memory_order_acquire))
        sched_yield();

    /* Keep all four producers racing until STOP is acknowledged. A send
     * that started before STOP may be admitted; its result is not presumed
     * to represent delivery or completion. */
    unsigned value = index;
    while (!atomic_load_explicit(&race->stop_admitted, memory_order_acquire)) {
        const int admitted = oreslang_source_actor_try_send_id(
            race->id, (int64_t)value);
        atomic_fetch_add_explicit(
            &race->attempts[index], 1, memory_order_release);
        if (admitted)
            atomic_fetch_add_explicit(
                &race->accepted[index], 1, memory_order_relaxed);
        ++value;
        if ((value & 63u) == 0)
            sched_yield();
    }

    /* This load follows publication after request_stop_id returned. Every
     * call in this loop starts strictly after the STOP admission point. */
    for (unsigned i = 0; i < POST_STOP_REJECTIONS; ++i) {
        require(!oreslang_source_actor_try_send_id(race->id, i));
        require(!oreslang_source_actor_try_request_stop_id(race->id));
        atomic_fetch_add_explicit(
            &race->post_stop_checked[index], 1, memory_order_relaxed);
    }
    return NULL;
}
static void await_ready(OresSourceActorId id) {
    for (unsigned spins = 0; spins < 2000000; ++spins) {
        const int state = oreslang_source_actor_ready_status_id(id);
        if (state == ORES_SOURCE_ACTOR_STARTUP_READY) return;
        require(state == ORES_SOURCE_ACTOR_STARTUP_PENDING ||
                state == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);
        sched_yield();
    }
    require(0);
}
static void await_producer_attempts(Race* race) {
    for (unsigned spins = 0; spins < 2000000; ++spins) {
        unsigned ready = 0;
        for (unsigned j = 0; j < PRODUCERS; ++j) {
            if (atomic_load_explicit(
                    &race->attempts[j], memory_order_acquire)
                    >= MIN_ATTEMPTS)
                ++ready;
        }
        if (ready == PRODUCERS) return;
        sched_yield();
    }
    require(0);
}

int main(void) {
    oreslang_source_actor_test_force_address_reuse(1);
    OresSourceActorId previous = 0;
    unsigned total_admitted = 0;
    for (unsigned generation = 0; generation < GENERATIONS; ++generation) {
        const OresSourceActorId id =
            oreslang_source_actor_spawn_id(initialize, receive, 1);
        require(id != 0 && id != UINT64_MAX && id > previous);
        require(oreslang_source_actor_test_address_reuses() == generation);
        await_ready(id);
        Race race = {.id = id};
        atomic_init(&race.go, 0);
        atomic_init(&race.stop_admitted, 0);
        pthread_t threads[PRODUCERS];
        Producer producers[PRODUCERS];
        for (unsigned j = 0; j < PRODUCERS; ++j) {
            atomic_init(&race.attempts[j], 0);
            atomic_init(&race.accepted[j], 0);
            atomic_init(&race.post_stop_checked[j], 0);
            producers[j] = (Producer){.race = &race, .index = j};
            require(pthread_create(
                &threads[j], NULL, run_producer, &producers[j]) == 0);
        }
        atomic_store_explicit(&race.go, 1, memory_order_release);
        await_producer_attempts(&race);
        /* The stop/send admission operations serialize on group.mutex.
         * The four producer threads remain live until this returns. */
        require(oreslang_source_actor_try_request_stop_id(id));
        atomic_store_explicit(
            &race.stop_admitted, 1, memory_order_release);
        for (unsigned j = 0; j < PRODUCERS; ++j) {
            require(pthread_join(threads[j], NULL) == 0);
            const unsigned attempts = atomic_load_explicit(
                &race.attempts[j], memory_order_acquire);
            require(attempts >= MIN_ATTEMPTS);
            require(atomic_load_explicit(
                &race.post_stop_checked[j], memory_order_acquire)
                == POST_STOP_REJECTIONS);
            total_admitted += atomic_load_explicit(
                &race.accepted[j], memory_order_relaxed);
        }
        oreslang_source_actor_join_done_id(id);
        require(oreslang_source_actor_ready_status_id(id) == -1);
        require(!oreslang_source_actor_try_send_id(id, 999));
        require(!oreslang_source_actor_try_request_stop_id(id));
        previous = id;
    }
    require(total_admitted > 0);
    require(atomic_load_explicit(&constructors, memory_order_relaxed)
            == GENERATIONS);
    require(oreslang_source_actor_test_address_reuses() == GENERATIONS - 1);
    oreslang_source_actor_test_force_address_reuse(0);
    printf("SOURCE_ACTOR_CHECKED_ID_STOP_SEND_RACE_PASS generations=%d producers=%d post_stop_rejections=%d\n",
           GENERATIONS, PRODUCERS,
           GENERATIONS * PRODUCERS * POST_STOP_REJECTIONS);
    return 0;
}
