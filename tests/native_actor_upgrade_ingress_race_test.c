#define _POSIX_C_SOURCE 200809L
/* Source-local native scheduler/upgrade admission linearization test.
 * The worker and host race while sharing one scheduler mailbox mutex.
 * This test is NOT a guest compiler-generated continuation proof. */
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct { atomic_int delivered; } State;
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    atomic_int go;
    OresNativeSendStatus status;
} Producer;
static void require(int ok) { if (!ok) abort(); }
static void on_message(void* raw, size_t actor, uint64_t message) {
    State* state = (State*)raw;
    if (actor != 0 || (message != 1 && message != 2)) abort();
    atomic_fetch_add(&state->delivered, 1);
}
static void* producer(void* raw) {
    Producer* p = (Producer*)raw;
    while (!atomic_load(&p->go)) sched_yield();
    p->status = ores_native_actor_scheduler_try_send(p->scheduler, p->actor, 1);
    return NULL;
}
int main(void) {
    unsigned admitted = 0, frozen = 0;
    for (unsigned round = 0; round < 128; ++round) {
        State state = {0};
        OresNativeActorScheduler* scheduler =
            ores_native_actor_scheduler_open(1, 2, 1, on_message, &state);
        require(scheduler != NULL);
        OresNativeActorRef actor =
            ores_native_actor_scheduler_spawn(scheduler, 0);
        require(actor.scheduler_domain != 0);
        Producer p = {.scheduler = scheduler, .actor = actor};
        pthread_t thread;
        require(pthread_create(&thread, NULL, producer, &p) == 0);
        atomic_store(&p.go, 1);
        uint64_t ticket = 0;
        require(ores_native_actor_scheduler_upgrade_freeze(
            scheduler, actor, 1, &ticket));
        require(pthread_join(thread, NULL) == 0);
        require(p.status == ORES_NATIVE_SEND_ACCEPTED ||
                p.status == ORES_NATIVE_SEND_UPGRADE_FROZEN);
        if (p.status == ORES_NATIVE_SEND_ACCEPTED) ++admitted;
        else ++frozen;
        /* All input surfaces share the SAME freeze. */
        require(!ores_native_actor_scheduler_submit(scheduler, 0, 2));
        require(ores_native_actor_scheduler_try_send(scheduler, actor, 2)
                == ORES_NATIVE_SEND_UPGRADE_FROZEN);
        int drained = 0;
        struct timespec pause = {0, 1000000};
        for (unsigned attempt = 0; attempt < 3000; ++attempt) {
            if (ores_native_actor_scheduler_upgrade_quiesced(
                    scheduler, actor, ticket)) {
                drained = 1;
                break;
            }
            nanosleep(&pause, NULL);
        }
        require(drained);
        require(atomic_load(&state.delivered) ==
                (p.status == ORES_NATIVE_SEND_ACCEPTED ? 1 : 0));
        require(ores_native_actor_scheduler_upgrade_finish(
                    scheduler, actor, ticket, 1));
        require(ores_native_actor_scheduler_upgrade_epoch(scheduler, actor) == 2);
        require(ores_native_actor_scheduler_try_send(scheduler, actor, 2)
                == ORES_NATIVE_SEND_ACCEPTED);
        require(ores_native_actor_scheduler_stop(scheduler, actor));
        require(ores_native_actor_scheduler_wait_done(scheduler, actor));
        require(atomic_load(&state.delivered) ==
                (p.status == ORES_NATIVE_SEND_ACCEPTED ? 2 : 1));
        require(!ores_native_actor_scheduler_upgrade_finish(
                    scheduler, actor, ticket, 0));
        ores_native_actor_scheduler_retire(scheduler);
        require(ores_native_actor_scheduler_drain(scheduler));
        require(ores_native_actor_scheduler_try_destroy(scheduler));
    }
    require(admitted + frozen == 128);
    printf("ACTOR_UPGRADE_INGRESS_RACE_PASS rounds=128 accepted=%u frozen=%u\n",
           admitted, frozen);
    return 0;
}
