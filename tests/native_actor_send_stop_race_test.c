#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { ROUNDS = 128, SENDS = 64 };
static void check(int condition) { if (!condition) abort(); }
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    pthread_barrier_t start;
    atomic_uint accepted;
    atomic_uint errors;
    atomic_uint completed;
} Race;

static void handler(void* raw, size_t slot, uint64_t payload) {
    Race* race = (Race*) raw;
    if (slot != 0 || payload > SENDS) atomic_fetch_add(&race->errors, 1);
    atomic_fetch_add(&race->completed, 1);
}
static void* sender(void* raw) {
    Race* race = (Race*) raw;
    (void) pthread_barrier_wait(&race->start);
    for (unsigned i = 1; i <= SENDS; ++i) {
        OresNativeSendStatus outcome =
            ores_native_actor_scheduler_try_send(race->scheduler, race->actor, i);
        if (outcome == ORES_NATIVE_SEND_ACCEPTED)
            atomic_fetch_add(&race->accepted, 1);
        else if (outcome != ORES_NATIVE_SEND_TERMINATED &&
                 outcome != ORES_NATIVE_SEND_MAILBOX_FULL)
            atomic_fetch_add(&race->errors, 1);
    }
    return NULL;
}
static void* stopper(void* raw) {
    Race* race = (Race*) raw;
    (void) pthread_barrier_wait(&race->start);
    if (!ores_native_actor_scheduler_stop(race->scheduler, race->actor))
        atomic_fetch_add(&race->errors, 1);
    /* Stop has definitely won the scheduler lock. No new send can succeed. */
    if (ores_native_actor_scheduler_try_send(race->scheduler, race->actor, SENDS)
            != ORES_NATIVE_SEND_TERMINATED)
        atomic_fetch_add(&race->errors, 1);
    return NULL;
}
int main(void) {
    for (unsigned round = 0; round < ROUNDS; ++round) {
        Race race = {0};
        race.scheduler = ores_native_actor_scheduler_open(1, SENDS + 1, 2, handler, &race);
        check(race.scheduler != NULL);
        race.actor = ores_native_actor_scheduler_spawn(race.scheduler, 0);
        check(race.actor.scheduler_domain != 0);
        check(ores_native_actor_scheduler_try_send(race.scheduler, race.actor, 0)
              == ORES_NATIVE_SEND_ACCEPTED);
        atomic_store(&race.accepted, 1);
        check(pthread_barrier_init(&race.start, NULL, 2) == 0);
        pthread_t send_thread, stop_thread;
        check(pthread_create(&send_thread, NULL, sender, &race) == 0);
        check(pthread_create(&stop_thread, NULL, stopper, &race) == 0);
        check(pthread_join(send_thread, NULL) == 0);
        check(pthread_join(stop_thread, NULL) == 0);
        check(pthread_barrier_destroy(&race.start) == 0);
        check(atomic_load(&race.errors) == 0);
        check(ores_native_actor_scheduler_try_send(race.scheduler, race.actor, 0)
              == ORES_NATIVE_SEND_TERMINATED);
        ores_native_actor_scheduler_retire(race.scheduler);
        check(ores_native_actor_scheduler_drain(race.scheduler));
        check(ores_native_actor_scheduler_poll_done(race.scheduler, race.actor));
        check(atomic_load(&race.completed) == atomic_load(&race.accepted));
        check(ores_native_actor_scheduler_completed(race.scheduler) ==
              atomic_load(&race.accepted));
        check(ores_native_actor_scheduler_try_destroy(race.scheduler));
    }
    puts("NATIVE_SEND_STOP_RACE_PASS");
    return 0;
}
