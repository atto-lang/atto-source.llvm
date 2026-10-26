#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeSelectArbiter.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) abort(); } while (0)

typedef struct {
    atomic_int block;
    atomic_int entered;
    atomic_uint observed_count;
    uint64_t observed[16];
} State;

static void handler(void* raw, size_t actor, uint64_t payload) {
    State* state = (State*)raw;
    CHECK(actor == 0);
    unsigned index = atomic_fetch_add_explicit(
        &state->observed_count, 1u, memory_order_acq_rel);
    CHECK(index < 16);
    state->observed[index] = payload;
    atomic_store_explicit(&state->entered, 1, memory_order_release);
    while (atomic_load_explicit(&state->block, memory_order_acquire))
        sched_yield();
}

static OresNativeActorScheduler* open_target(
        State* state, OresNativeActorRef* target) {
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(1, 1, 1, handler, state);
    CHECK(scheduler != NULL);
    *target = ores_native_actor_scheduler_spawn(scheduler, 0);
    CHECK(target->scheduler_domain != 0);
    return scheduler;
}

static void wait_entered(State* state) {
    while (!atomic_load_explicit(&state->entered, memory_order_acquire))
        sched_yield();
}

static void expect_send_state(
        OresNativeActorScheduler* scheduler,
        OresNativeAsyncSendTicket ticket,
        OresNativeAsyncSendState expected_state,
        OresNativeSendStatus expected_outcome) {
    OresNativeAsyncSendState state = ORES_NATIVE_ASYNC_SEND_INVALID;
    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_async_send_status(
        scheduler, ticket, &state, &outcome));
    CHECK(state == expected_state);
    CHECK(outcome == expected_outcome);
}

static void close_target(
        OresNativeActorScheduler* scheduler,
        OresNativeActorRef target) {
    CHECK(ores_native_actor_scheduler_stop(scheduler, target));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void losing_select_arm_never_publishes(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 1)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 101, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 2, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_actor_scheduler_cancel_async_send(scheduler, send));
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_CANCELLED, ORES_NATIVE_SEND_CANCELLED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));
    CHECK(ores_native_select_active_tokens(&select) == 0);

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 1);
    close_target(scheduler, target);
}

static void mailbox_credit_commits_send_winner_before_enqueue(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 10)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 102, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 11, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    atomic_store_explicit(&state.block, 0, memory_order_release);
    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, send, &outcome));
    CHECK(outcome == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);

    uint64_t winner = UINT64_MAX;
    CHECK(ores_native_select_winner(&select, &winner));
    CHECK(winner == 0);
    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_LOST);
    CHECK(ores_native_select_active_tokens(&select) == 0);

    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 2);
    CHECK(state.observed[0] == 10);
    CHECK(state.observed[1] == 11);
    close_target(scheduler, target);
}

static void stop_failure_can_win_selection_without_payload_publication(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 20)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 103, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 21, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, target));
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_FAILED, ORES_NATIVE_SEND_TERMINATED);

    uint64_t winner = UINT64_MAX;
    CHECK(ores_native_select_winner(&select, &winner));
    CHECK(winner == 0);
    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_LOST);
    CHECK(ores_native_select_active_tokens(&select) == 0);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 20);
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void prior_other_winner_turns_stop_failure_into_loser_cancel(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 30)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 104, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 31, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_WON);

    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, target));
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_CANCELLED, ORES_NATIVE_SEND_CANCELLED);
    CHECK(ores_native_select_active_tokens(&select) == 0);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void immediate_credit_uses_same_winner_protocol(void) {
    State state = {0};
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 105, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 41, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);

    uint64_t winner = UINT64_MAX;
    CHECK(ores_native_select_winner(&select, &winner));
    CHECK(winner == 0);
    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_LOST);
    CHECK(ores_native_select_active_tokens(&select) == 0);

    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 41);
    close_target(scheduler, target);
}

static void registration_exhaustion_does_not_win_selection(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 60)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeAsyncSendTicket pending[ORES_NATIVE_ASYNC_SENDS_PER_ACTOR];
    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        pending[i] = (OresNativeAsyncSendTicket){0};
        CHECK(ores_native_actor_scheduler_submit_async(
            scheduler, target, UINT64_C(1000) + i, &pending[i])
            == ORES_NATIVE_SEND_ACCEPTED);
        expect_send_state(
            scheduler, pending[i],
            ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);
    }

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 107, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket refused = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 61, send_arm, &refused)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    CHECK(refused.generation == 0);
    CHECK(ores_native_select_active_tokens(&select) == 1);
    uint64_t winner = UINT64_MAX;
    CHECK(!ores_native_select_winner(&select, &winner));

    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_select_active_tokens(&select) == 0);
    CHECK(ores_native_select_winner(&select, &winner));
    CHECK(winner == 1);

    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        CHECK(ores_native_actor_scheduler_cancel_async_send(
            scheduler, pending[i]));
        CHECK(ores_native_actor_scheduler_release_async_send(
            scheduler, pending[i]));
    }

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 60);
    close_target(scheduler, target);
}

static void winner_race_beats_registration_exhaustion(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 63)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeAsyncSendTicket pending[ORES_NATIVE_ASYNC_SENDS_PER_ACTOR];
    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        pending[i] = (OresNativeAsyncSendTicket){0};
        CHECK(ores_native_actor_scheduler_submit_async(
            scheduler, target, 10000 + i, &pending[i])
            == ORES_NATIVE_SEND_ACCEPTED);
    }

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 109, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    /* Selection completes while the send arm is still only a registration.
     * Descriptor exhaustion must therefore classify this arm as a loser,
     * not overwrite an already-completed selection with setup failure. */
    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_WON);

    OresNativeAsyncSendTicket refused = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 64, send_arm, &refused)
        == ORES_NATIVE_SEND_CANCELLED);
    CHECK(refused.generation == 0);
    CHECK(ores_native_select_active_tokens(&select) == 0);

    uint64_t winner = UINT64_MAX;
    CHECK(ores_native_select_winner(&select, &winner));
    CHECK(winner == 1);

    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        CHECK(ores_native_actor_scheduler_cancel_async_send(
            scheduler, pending[i]));
        CHECK(ores_native_actor_scheduler_release_async_send(
            scheduler, pending[i]));
    }

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 63);
    close_target(scheduler, target);
}

static void immediate_terminated_target_is_terminal_select_event(void) {
    State state = {0};
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, target));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 108, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket none = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 62, send_arm, &none)
        == ORES_NATIVE_SEND_TERMINATED);
    CHECK(none.generation == 0);

    uint64_t winner = UINT64_MAX;
    CHECK(ores_native_select_winner(&select, &winner));
    CHECK(winner == 0);
    CHECK(ores_native_select_try_commit(other_arm) == ORES_NATIVE_SELECT_LOST);
    CHECK(ores_native_select_active_tokens(&select) == 0);

    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void scheduler_bound_send_token_cannot_commit_early(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 60)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 107, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 61, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    /*
     * submit_selectable_async transferred arm authority into the scheduler.
     * A coordinator retaining a copied token cannot claim the send winner
     * before real mailbox credit exists.
     */
    CHECK(ores_native_select_try_commit(send_arm)
        == ORES_NATIVE_SELECT_INVALID);
    CHECK(ores_native_select_try_commit(other_arm)
        == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_actor_scheduler_cancel_async_send(scheduler, send));
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));
    CHECK(ores_native_select_active_tokens(&select) == 0);

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 60);
    close_target(scheduler, target);
}

static void sealed_selection_never_publishes_pending_send(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 50)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 106, 2));
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    CHECK(ores_native_select_register(&select, 0, &send_arm));
    CHECK(ores_native_select_register(&select, 1, &other_arm));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 51, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    CHECK(ores_native_select_seal(&select));
    CHECK(ores_native_select_active_tokens(&select) == 0);

    /* Even if the coordinator has not yet issued source-specific loser
     * cancellation, later target credit must observe the sealed generation
     * and cancel the descriptor rather than publish its payload. */
    atomic_store_explicit(&state.block, 0, memory_order_release);
    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, send, &outcome));
    CHECK(outcome == ORES_NATIVE_SEND_CANCELLED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_CANCELLED, ORES_NATIVE_SEND_CANCELLED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 50);
    close_target(scheduler, target);
}

static void stale_bound_descriptor_cannot_commit_rearmed_generation(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef target = {0};
    OresNativeActorScheduler* scheduler = open_target(&state, &target);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 70)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 500, 2));
    OresNativeSelectToken stale_send = {0}, stale_other = {0};
    CHECK(ores_native_select_register(&select, 0, &stale_send));
    CHECK(ores_native_select_register(&select, 1, &stale_other));

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 71, stale_send, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    CHECK(ores_native_select_seal(&select));
    CHECK(ores_native_select_active_tokens(&select) == 0);
    CHECK(ores_native_select_arbiter_rearm(&select, 501, 2));

    OresNativeSelectToken current_a = {0}, current_b = {0};
    CHECK(ores_native_select_register(&select, 0, &current_a));
    CHECK(ores_native_select_register(&select, 1, &current_b));

    /* Credit release services the stale physical descriptor after rearm.
     * Its old generation must only cancel itself; it cannot claim arm 0 of
     * generation 501. */
    atomic_store_explicit(&state.block, 0, memory_order_release);
    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, send, &outcome));
    CHECK(outcome == ORES_NATIVE_SEND_CANCELLED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));

    CHECK(ores_native_select_try_commit(current_b)
        == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_select_try_commit(current_a)
        == ORES_NATIVE_SELECT_LOST);
    uint64_t winner = UINT64_MAX;
    CHECK(ores_native_select_winner(&select, &winner));
    CHECK(winner == 1);
    CHECK(ores_native_select_active_tokens(&select) == 0);

    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 70);
    close_target(scheduler, target);
}

typedef struct {
    OresNativeSelectToken token;
    atomic_int* go;
    OresNativeSelectCommitResult result;
} CommitRace;

static void* commit_race_thread(void* raw) {
    CommitRace* race = (CommitRace*)raw;
    while (!atomic_load_explicit(race->go, memory_order_acquire))
        sched_yield();
    race->result = ores_native_select_try_commit(race->token);
    return NULL;
}

static void concurrent_arms_have_exactly_one_winner(void) {
    for (uint64_t round = 0; round < 512; ++round) {
        OresNativeSelectArbiter select = {0};
        CHECK(ores_native_select_arbiter_init(
            &select, UINT64_C(1000) + round, 2));

        OresNativeSelectToken a = {0}, b = {0};
        CHECK(ores_native_select_register(&select, 0, &a));
        CHECK(ores_native_select_register(&select, 1, &b));

        atomic_int go = ATOMIC_VAR_INIT(0);
        CommitRace left = {a, &go, ORES_NATIVE_SELECT_INVALID};
        CommitRace right = {b, &go, ORES_NATIVE_SELECT_INVALID};
        pthread_t ta, tb;
        CHECK(pthread_create(&ta, NULL, commit_race_thread, &left) == 0);
        CHECK(pthread_create(&tb, NULL, commit_race_thread, &right) == 0);
        atomic_store_explicit(&go, 1, memory_order_release);
        CHECK(pthread_join(ta, NULL) == 0);
        CHECK(pthread_join(tb, NULL) == 0);

        const unsigned wins =
            (left.result == ORES_NATIVE_SELECT_WON)
            + (right.result == ORES_NATIVE_SELECT_WON);
        const unsigned losses =
            (left.result == ORES_NATIVE_SELECT_LOST)
            + (right.result == ORES_NATIVE_SELECT_LOST);
        CHECK(wins == 1);
        CHECK(losses == 1);
        CHECK(ores_native_select_active_tokens(&select) == 0);
        uint64_t winner = UINT64_MAX;
        CHECK(ores_native_select_winner(&select, &winner));
        CHECK(winner < 2);
    }
}

static void rearm_generation_is_strictly_monotonic(void) {
    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 300, 1));

    OresNativeSelectToken token = {0};
    CHECK(ores_native_select_register(&select, 0, &token));
    CHECK(ores_native_select_try_commit(token) == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_select_active_tokens(&select) == 0);

    CHECK(!ores_native_select_arbiter_rearm(&select, 300, 1));
    CHECK(!ores_native_select_arbiter_rearm(&select, 299, 1));
    CHECK(ores_native_select_arbiter_rearm(&select, 301, 1));

    OresNativeSelectToken next = {0};
    CHECK(ores_native_select_register(&select, 0, &next));
    CHECK(next.selection_generation == 301);
    CHECK(ores_native_select_try_commit(next) == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_select_active_tokens(&select) == 0);
}

static void stale_select_generation_fails_closed(void) {
    OresNativeSelectArbiter select = {0};
    CHECK(ores_native_select_arbiter_init(&select, 200, 2));

    OresNativeSelectToken stale = {0};
    CHECK(ores_native_select_register(&select, 0, &stale));
    CHECK(ores_native_select_detach(stale));
    CHECK(ores_native_select_active_tokens(&select) == 0);

    /* OPEN generations cannot be silently reset. Finalization must first
     * make the prior generation terminal, then rearm owns the reset gate. */
    CHECK(!ores_native_select_arbiter_rearm(&select, 201, 2));
    CHECK(ores_native_select_seal(&select));
    CHECK(ores_native_select_arbiter_rearm(&select, 201, 2));
    OresNativeSelectToken current = {0};
    CHECK(ores_native_select_register(&select, 0, &current));
    CHECK(ores_native_select_try_commit(stale) == ORES_NATIVE_SELECT_INVALID);
    CHECK(ores_native_select_try_commit(current) == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_select_active_tokens(&select) == 0);
}

int main(void) {
    losing_select_arm_never_publishes();
    mailbox_credit_commits_send_winner_before_enqueue();
    stop_failure_can_win_selection_without_payload_publication();
    prior_other_winner_turns_stop_failure_into_loser_cancel();
    immediate_credit_uses_same_winner_protocol();
    registration_exhaustion_does_not_win_selection();
    winner_race_beats_registration_exhaustion();
    immediate_terminated_target_is_terminal_select_event();
    scheduler_bound_send_token_cannot_commit_early();
    sealed_selection_never_publishes_pending_send();
    stale_bound_descriptor_cannot_commit_rearmed_generation();
    concurrent_arms_have_exactly_one_winner();
    rearm_generation_is_strictly_monotonic();
    stale_select_generation_fails_closed();
    puts("NATIVE_SELECTABLE_SEND_PASS loser_no_publish=1 credit_commit=1 stop_terminal=1 stop_after_other=1 immediate=1 registration_exhaustion_not_winner=1 exhaustion_after_winner=cancelled monotonic_generation=1 immediate_terminated=1 scheduler_token_handoff=1 sealed_no_publish=1 stale_bound_rearm=1 concurrent_winner=512 stale_generation=1");
    return 0;
}
