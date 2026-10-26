#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) abort(); } while (0)

static atomic_int first_entered;
static atomic_int release_first;
static atomic_int loser_payload_seen;
static atomic_int continuation_seen;

static void init_actor(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    (void)domain;
    (void)epoch;
}

static int64_t blocking_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap;
    (void)domain;
    (void)epoch;
    if (payload == 901)
        atomic_store_explicit(
            &loser_payload_seen, 1, memory_order_release);
    atomic_store_explicit(&first_entered, 1, memory_order_release);
    while (!atomic_load_explicit(
            &release_first, memory_order_acquire))
        sched_yield();
    return payload;
}

static int64_t passthrough_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap;
    (void)domain;
    (void)epoch;
    return payload;
}

static void wait_ready(void* actor) {
    for (;;) {
        const int state = oreslang_source_actor_ready_status(actor);
        if (state == ORES_SOURCE_ACTOR_STARTUP_READY) return;
        CHECK(state == ORES_SOURCE_ACTOR_STARTUP_PENDING
            || state == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);
        sched_yield();
    }
}

static void saturate_first_mailbox(void* actor) {
    CHECK(oreslang_source_actor_try_send_checked(actor, 700)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (!atomic_load_explicit(
            &first_entered, memory_order_acquire))
        sched_yield();

    int full = 0;
    for (int i = 0; i < 512; ++i) {
        const int status =
            oreslang_source_actor_try_send_checked(actor, 710 + i);
        if (status == ORES_NATIVE_SEND_MAILBOX_FULL) {
            full = 1;
            break;
        }
        CHECK(status == ORES_NATIVE_SEND_ACCEPTED);
    }
    CHECK(full);
}

static atomic_uint fair_continuations;

static int64_t fair_selected(
        void* actor0, void* actor1, uint64_t winner_arm) {
    CHECK(actor0 != NULL && actor1 != NULL && actor0 != actor1);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    CHECK(winner_arm <= 1);
    atomic_fetch_add_explicit(
        &fair_continuations, 1u, memory_order_acq_rel);
    return 40 + (int64_t)winner_arm;
}

static int64_t selected(
        void* actor0, void* actor1, uint64_t winner_arm) {
    CHECK(actor0 != NULL && actor1 != NULL && actor0 != actor1);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    CHECK(winner_arm == 1);
    atomic_store_explicit(
        &continuation_seen, 1, memory_order_release);

    /* Release the physically active losing target before the external-host
     * finalizer applies STOP and waits for its terminal acknowledgement. */
    atomic_store_explicit(&release_first, 1, memory_order_release);
    return 43;
}

int main(void) {
    atomic_init(&first_entered, 0);
    atomic_init(&release_first, 0);
    atomic_init(&loser_payload_seen, 0);
    atomic_init(&continuation_seen, 0);

    void* actor0 = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    void* actor1 = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor0 != NULL && actor1 != NULL && actor0 != actor1);
    wait_ready(actor0);
    wait_ready(actor1);

    /* Arm 0 cannot acquire USER credit. Arm 1 therefore has to win even
     * though arm 0 is registered/submitted first. */
    saturate_first_mailbox(actor0);

    CHECK(oreslang_source_actor_detach_select_send2_checked(
        actor0, 901,
        actor1, 902,
        selected,
        UINT64_C(0x6e01))
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    uint64_t winner = UINT64_MAX;
    int64_t result = 0;
    CHECK(oreslang_source_actor_wait_detached_select_send2_result(
        &winner, &result)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(winner == 1);
    CHECK(result == 43);
    CHECK(atomic_load_explicit(
        &continuation_seen, memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(
        &loser_payload_seen, memory_order_acquire) == 0);

    fprintf(stderr,
        "SOURCE_SELECT_SEND2_RUNTIME_PASS winner=1 loser_payload=0 "
        "root_continuation=1 physical_cleanup=1\n");

    /* Keep four actors live in one runtime so the FAIR cursor survives the
     * first pair's finalization. With both arms immediately admissible, the
     * first select starts at arm 0 and the next starts at arm 1. */
    atomic_init(&fair_continuations, 0u);
    void* a0 = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    void* a1 = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    void* b0 = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    void* b1 = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(a0 && a1 && b0 && b1);
    wait_ready(a0); wait_ready(a1); wait_ready(b0); wait_ready(b1);

    CHECK(oreslang_source_actor_detach_select_send2_checked(
        a0, 1001, a1, 1002, fair_selected, UINT64_C(0x6e02))
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    winner = UINT64_MAX;
    result = 0;
    CHECK(oreslang_source_actor_wait_detached_select_send2_result(
        &winner, &result) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(winner == 0);
    CHECK(result == 40);

    CHECK(oreslang_source_actor_detach_select_send2_checked(
        b0, 1003, b1, 1004, fair_selected, UINT64_C(0x6e03))
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    winner = UINT64_MAX;
    result = 0;
    CHECK(oreslang_source_actor_wait_detached_select_send2_result(
        &winner, &result) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(winner == 1);
    CHECK(result == 41);
    CHECK(atomic_load_explicit(
        &fair_continuations, memory_order_acquire) == 2u);

    fprintf(stderr,
        "SOURCE_SELECT_SEND2_FAIR_PASS immediate_winners=0,1 "
        "round_robin=1\n");
    return 0;
}
