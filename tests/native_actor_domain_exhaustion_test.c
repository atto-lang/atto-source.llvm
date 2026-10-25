#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include "test_barrier.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

enum { ATTEMPTS = 12, ROUNDS = 24, REMAINING_IDS = 3 };
void ores_native_actor_scheduler_test_set_next_domain(uint64_t value);

static void require(int condition) { if (!condition) abort(); }
static void ignore_message(void* unused, size_t actor, uint64_t payload) {
    (void) unused;
    (void) actor;
    (void) payload;
}
typedef struct {
    OresTestBarrier* barrier;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef ref;
} Attempt;
static void* try_open(void* data) {
    Attempt* attempt = (Attempt*)data;
    (void)ores_test_barrier_wait(attempt->barrier);
    attempt->scheduler = ores_native_actor_scheduler_open(
        1, 1, 1, ignore_message, NULL);
    if (attempt->scheduler)
        attempt->ref = ores_native_actor_scheduler_spawn(
            attempt->scheduler, 0);
    return NULL;
}
int main(void) {
    for (int round = 0; round < ROUNDS; ++round) {
        OresTestBarrier barrier;
        require(ores_test_barrier_init(&barrier, ATTEMPTS) == 0);
        pthread_t workers[ATTEMPTS];
        Attempt attempts[ATTEMPTS] = {0};
        ores_native_actor_scheduler_test_set_next_domain(UINT64_MAX - REMAINING_IDS);
        for (int i = 0; i < ATTEMPTS; ++i) {
            attempts[i].barrier = &barrier;
            require(pthread_create(&workers[i], NULL, try_open, &attempts[i]) == 0);
        }
        for (int i = 0; i < ATTEMPTS; ++i)
            require(pthread_join(workers[i], NULL) == 0);
        require(ores_test_barrier_destroy(&barrier) == 0);

        uint64_t domains[REMAINING_IDS] = {0};
        int accepted = 0;
        for (int i = 0; i < ATTEMPTS; ++i) {
            if (!attempts[i].scheduler) {
                require(attempts[i].ref.scheduler_domain == 0);
                continue;
            }
            require(accepted < REMAINING_IDS);
            uint64_t domain = attempts[i].ref.scheduler_domain;
            require(domain >= UINT64_MAX - REMAINING_IDS && domain < UINT64_MAX);
            for (int j = 0; j < accepted; ++j)
                require(domain != domains[j]);
            domains[accepted++] = domain;
        }
        require(accepted == REMAINING_IDS);
        /* Even repeated new attempts cannot revive IDs 0 or 1 by overflow. */
        for (int i = 0; i < ATTEMPTS; ++i)
            require(ores_native_actor_scheduler_open(1, 1, 1, ignore_message, NULL) == NULL);
        for (int i = 0; i < ATTEMPTS; ++i) {
            OresNativeActorScheduler* scheduler = attempts[i].scheduler;
            if (!scheduler) continue;
            OresNativeActorRef ref = attempts[i].ref;
            require(ores_native_actor_scheduler_poll_ready(scheduler, ref));
            require(ores_native_actor_scheduler_stop(scheduler, ref));
            require(ores_native_actor_scheduler_poll_done(scheduler, ref));
            ores_native_actor_scheduler_retire(scheduler);
            require(ores_native_actor_scheduler_drain(scheduler));
            require(ores_native_actor_scheduler_try_destroy(scheduler));
        }
    }
    puts("NATIVE_ACTOR_DOMAIN_EXHAUSTION_PASS");
    return 0;
}
