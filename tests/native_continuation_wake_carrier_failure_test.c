#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, \
    "NATIVE_WAKE_CARRIER_FAILURE at line %d\\n", __LINE__); abort(); } } while (0)
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    atomic_int wakes, errors;
} Fixture;
static void data(void* raw, size_t actor, uint64_t payload) {
    Fixture* f = (Fixture*)raw;
    if (actor || payload != 99
            || !ores_native_actor_scheduler_is_current_actor(
                f->scheduler, f->actor))
        atomic_fetch_add(&f->errors, 1);
}
static void wake(void* raw, size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    Fixture* f = (Fixture*)raw;
    if (actor || outcome != ORES_NATIVE_WAKE_READY
            || !ores_native_actor_scheduler_is_current_wake(f->scheduler, ticket))
        atomic_fetch_add(&f->errors, 1);
    atomic_fetch_add(&f->wakes, 1);
}
int main(void) {
    Fixture f = {0};
    f.scheduler = ores_native_actor_scheduler_open_with_wakes(
        1, 2, 1, data, NULL, NULL, wake, &f);
    REQUIRE(f.scheduler);
    f.actor = ores_native_actor_scheduler_spawn(f.scheduler, 0);
    REQUIRE(f.actor.scheduler_domain);
    REQUIRE(ores_native_actor_scheduler_started_carriers(f.scheduler) == 0);
    OresNativeWakeTicket ticket = {0};
    /* The first worker-start fails. No ticket can be retained or transferred
     * because later publication must be non-failing after credit transfer. */
    ores_native_actor_scheduler_test_fail_next_create();
    REQUIRE(!ores_native_actor_scheduler_reserve_wake(
        f.scheduler, f.actor, f.actor, 1, 2, 3, 4, &ticket));
    REQUIRE(!ticket.slot_generation);
    REQUIRE(!ores_native_actor_scheduler_outstanding_wake_loans(
        f.scheduler, f.actor));
    REQUIRE(ores_native_actor_scheduler_started_carriers(f.scheduler) == 0);
    REQUIRE(ores_native_actor_scheduler_reserve_wake(
        f.scheduler, f.actor, f.actor, 1, 2, 3, 4, &ticket));
    REQUIRE(ores_native_actor_scheduler_started_carriers(f.scheduler) == 1);
    REQUIRE(ores_native_actor_scheduler_transfer_wake(f.scheduler, ticket));
    REQUIRE(ores_native_actor_scheduler_publish_wake(
        f.scheduler, ticket, ORES_NATIVE_WAKE_READY) == ORES_NATIVE_WAKE_PUBLISHED);
    REQUIRE(ores_native_actor_scheduler_send(f.scheduler, f.actor, 99));
    REQUIRE(ores_native_actor_scheduler_drain(f.scheduler));
    REQUIRE(atomic_load(&f.wakes) == 1 && atomic_load(&f.errors) == 0);
    REQUIRE(ores_native_actor_scheduler_stop(f.scheduler, f.actor));
    REQUIRE(ores_native_actor_scheduler_wait_done(f.scheduler, f.actor));
    ores_native_actor_scheduler_retire(f.scheduler);
    REQUIRE(ores_native_actor_scheduler_try_destroy(f.scheduler));
    /* Execution ID exhaustion cannot allow ordinary DATA to consume a
     * previously reserved wake's final execution/acceptance credit. */
    Fixture exhausted = {0};
    exhausted.scheduler = ores_native_actor_scheduler_open_with_wakes(
        1, 2, 1, data, NULL, NULL, wake, &exhausted);
    REQUIRE(exhausted.scheduler);
    exhausted.actor = ores_native_actor_scheduler_spawn(exhausted.scheduler, 0);
    REQUIRE(exhausted.actor.scheduler_domain);
    REQUIRE(ores_native_actor_scheduler_test_set_next_execution(
        exhausted.scheduler, UINT64_MAX));
    OresNativeWakeTicket last = {0};
    REQUIRE(ores_native_actor_scheduler_reserve_wake(
        exhausted.scheduler, exhausted.actor, exhausted.actor,
        11, 12, 13, 14, &last));
    REQUIRE(ores_native_actor_scheduler_try_send(
        exhausted.scheduler, exhausted.actor, 99)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    REQUIRE(ores_native_actor_scheduler_transfer_wake(
        exhausted.scheduler, last));
    REQUIRE(ores_native_actor_scheduler_publish_wake(
        exhausted.scheduler, last, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PUBLISHED);
    REQUIRE(ores_native_actor_scheduler_drain(exhausted.scheduler));
    REQUIRE(atomic_load(&exhausted.wakes) == 1);
    REQUIRE(!atomic_load(&exhausted.errors));
    REQUIRE(!ores_native_actor_scheduler_reserve_wake(
        exhausted.scheduler, exhausted.actor, exhausted.actor,
        21, 22, 23, 24, &last));
    REQUIRE(ores_native_actor_scheduler_stop(
        exhausted.scheduler, exhausted.actor));
    REQUIRE(ores_native_actor_scheduler_wait_done(
        exhausted.scheduler, exhausted.actor));
    ores_native_actor_scheduler_retire(exhausted.scheduler);
    REQUIRE(ores_native_actor_scheduler_try_destroy(exhausted.scheduler));

    puts("NATIVE_WAKE_CARRIER_FAILURE_FENCE_PASS");
    return 0;
}
