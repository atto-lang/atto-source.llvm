#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <sched.h>
#include "test_barrier.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { ROUNDS = 128, SENDS = 64, PREFILL = 8 };

static void check(int condition) {
    if (!condition) abort();
}

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresTestBarrier start;
    atomic_uint accepted;
    atomic_uint errors;
    atomic_uint completed;
    atomic_uint executing_handlers;
    atomic_int first_entered;
    atomic_int release_first;
    atomic_uint admitted[SENDS + 1];
    atomic_uint observed[SENDS + 1];
} Race;

typedef struct {
    Race* race;
    unsigned first, last, yield_salt;
} RaceSender;

static void handler(void* raw, size_t slot, uint64_t payload) {
    Race* race = (Race*)raw;
    if (atomic_fetch_add(&race->executing_handlers, 1) != 0)
        atomic_fetch_add(&race->errors, 1);
    if (slot != 0 || payload > SENDS) {
        atomic_fetch_add(&race->errors, 1);
        atomic_fetch_sub(&race->executing_handlers, 1);
        return;
    }

    if (payload == 0) {
        atomic_store_explicit(
            &race->first_entered, 1, memory_order_release);
        while (!atomic_load_explicit(
                &race->release_first, memory_order_acquire))
            sched_yield();
    }

    if (atomic_fetch_add(&race->observed[payload], 1) != 0)
        atomic_fetch_add(&race->errors, 1);
    atomic_fetch_add(&race->completed, 1);
    atomic_fetch_sub(&race->executing_handlers, 1);
}

static void* sender(void* raw) {
    RaceSender* producer = (RaceSender*)raw;
    Race* race = producer->race;
    (void)ores_test_barrier_wait(&race->start);
    for (unsigned i = producer->first; i <= producer->last; ++i) {
        if (((i + producer->yield_salt) & 3u) == 0u) sched_yield();
        OresNativeSendStatus outcome =
            ores_native_actor_scheduler_try_send(
                race->scheduler, race->actor, i);
        if (outcome == ORES_NATIVE_SEND_ACCEPTED) {
            atomic_store(&race->admitted[i], 1);
            atomic_fetch_add(&race->accepted, 1);
        } else if (outcome != ORES_NATIVE_SEND_TERMINATED
                && outcome != ORES_NATIVE_SEND_MAILBOX_FULL) {
            atomic_fetch_add(&race->errors, 1);
        }
    }
    return NULL;
}

static void* stopper(void* raw) {
    Race* race = (Race*)raw;
    (void)ores_test_barrier_wait(&race->start);
    if (!ores_native_actor_scheduler_stop_sealed(
            race->scheduler, race->actor))
        atomic_fetch_add(&race->errors, 1);

    if (ores_native_actor_scheduler_try_send(
            race->scheduler, race->actor, SENDS)
            != ORES_NATIVE_SEND_TERMINATED)
        atomic_fetch_add(&race->errors, 1);

    atomic_store_explicit(
        &race->release_first, 1, memory_order_release);
    return NULL;
}

int main(void) {
    for (unsigned round = 0; round < ROUNDS; ++round) {
        Race race = {0};
        race.scheduler = ores_native_actor_scheduler_open(
            1, SENDS + 1, 2, handler, &race);
        check(race.scheduler != NULL);
        race.actor =
            ores_native_actor_scheduler_spawn(race.scheduler, 0);
        check(race.actor.scheduler_domain != 0);

        for (unsigned payload = 0; payload <= PREFILL; ++payload) {
            check(ores_native_actor_scheduler_try_send(
                race.scheduler, race.actor, payload)
                == ORES_NATIVE_SEND_ACCEPTED);
            atomic_store(&race.admitted[payload], 1);
            atomic_fetch_add(&race.accepted, 1);
        }

        while (!atomic_load_explicit(
                &race.first_entered, memory_order_acquire))
            sched_yield();

        check(ores_test_barrier_init(&race.start, 3) == 0);
        RaceSender first = {
            &race,
            PREFILL + 1,
            PREFILL + (SENDS - PREFILL) / 2,
            round & 3u
        };
        RaceSender second = {
            &race,
            first.last + 1,
            SENDS,
            (round >> 2) & 3u
        };

        pthread_t producer_a, producer_b, stop_thread;
        check(pthread_create(&producer_a, NULL, sender, &first) == 0);
        check(pthread_create(&producer_b, NULL, sender, &second) == 0);
        check(pthread_create(&stop_thread, NULL, stopper, &race) == 0);
        check(pthread_join(producer_a, NULL) == 0);
        check(pthread_join(producer_b, NULL) == 0);
        check(pthread_join(stop_thread, NULL) == 0);
        check(ores_test_barrier_destroy(&race.start) == 0);

        check(ores_native_actor_scheduler_wait_done(
            race.scheduler, race.actor));
        check(atomic_load(&race.errors) == 0);
        check(atomic_load(&race.executing_handlers) == 0);
        check(atomic_load(&race.completed) == 1);
        check(atomic_load(&race.observed[0]) == 1);

        unsigned distinct_admitted = 0;
        unsigned distinct_observed = 0;
        for (unsigned payload = 0; payload <= SENDS; ++payload) {
            const unsigned admitted =
                atomic_load(&race.admitted[payload]);
            const unsigned observed =
                atomic_load(&race.observed[payload]);
            check(admitted <= 1);
            check(observed <= admitted);
            distinct_admitted += admitted;
            distinct_observed += observed;
        }
        check(distinct_admitted == atomic_load(&race.accepted));
        check(distinct_observed == 1);
        check(distinct_admitted >= PREFILL + 1);

        OresNativeActorTerminalState terminal = {0};
        check(ores_native_actor_scheduler_terminal_state(
            race.scheduler, race.actor, &terminal));
        check(terminal.kind == ORES_NATIVE_TERMINATION_STOPPED);
        check(terminal.accepted_user_messages == distinct_admitted);
        check(terminal.processed_user_messages == 1);
        check(terminal.abandoned_user_messages
            == distinct_admitted - 1);
        check(terminal.processed_user_messages
            + terminal.abandoned_user_messages
            == terminal.accepted_user_messages);
        check(terminal.abandoned_control_messages == 0);

        check(ores_native_actor_scheduler_completed(
            race.scheduler) == 1);

        ores_native_actor_scheduler_retire(race.scheduler);
        check(ores_native_actor_scheduler_drain(race.scheduler));
        check(ores_native_actor_scheduler_try_destroy(race.scheduler));
    }

    puts("NATIVE_SEALED_SEND_STOP_RACE_PASS");
    return 0;
}
