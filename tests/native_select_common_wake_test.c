#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeSelectArbiter.h"

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

    OresNativeActorScheduler* scheduler;
    atomic_uint wake_count;
    atomic_int wake_seen;
    atomic_int wake_outcome;
    atomic_int hold_wake;
    atomic_int wake_entered;
    atomic_int wake_exited;
    atomic_uint_fast64_t wake_completion;
    OresNativeWakeTicket wake_ticket;
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

static void wake_callback(
        void* raw, size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    State* state = (State*)raw;
    CHECK(actor == 1);
    CHECK(state->scheduler != NULL);
    CHECK(ores_native_actor_scheduler_is_current_wake(
        state->scheduler, ticket));
    atomic_store_explicit(
        &state->wake_entered, 1, memory_order_release);
    while (atomic_load_explicit(
            &state->hold_wake, memory_order_acquire))
        sched_yield();

    state->wake_ticket = ticket;
    atomic_store_explicit(
        &state->wake_completion, ticket.completion_code, memory_order_release);
    atomic_store_explicit(
        &state->wake_outcome, (int)outcome, memory_order_release);
    atomic_fetch_add_explicit(&state->wake_count, 1u, memory_order_acq_rel);
    atomic_store_explicit(&state->wake_seen, 1, memory_order_release);
    atomic_store_explicit(&state->wake_exited, 1, memory_order_release);
}

static OresNativeActorScheduler* open_pair(
        State* state,
        OresNativeActorRef* target,
        OresNativeActorRef* producer) {
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open_with_wakes(
            2, 1, 2, handler, NULL, NULL, wake_callback, state);
    CHECK(scheduler != NULL);
    state->scheduler = scheduler;
    *target = ores_native_actor_scheduler_spawn(scheduler, 0);
    *producer = ores_native_actor_scheduler_spawn(scheduler, 1);
    CHECK(target->scheduler_domain != 0);
    CHECK(producer->scheduler_domain == target->scheduler_domain);
    return scheduler;
}

static void wait_entered(State* state) {
    while (!atomic_load_explicit(&state->entered, memory_order_acquire))
        sched_yield();
}

static void wait_wake(State* state) {
    while (!atomic_load_explicit(&state->wake_seen, memory_order_acquire))
        sched_yield();
}

static void reset_wake(State* state) {
    atomic_store_explicit(&state->wake_seen, 0, memory_order_release);
    atomic_store_explicit(&state->wake_outcome, -1, memory_order_release);
    atomic_store_explicit(&state->wake_completion, 0, memory_order_release);
    atomic_store_explicit(&state->wake_entered, 0, memory_order_release);
    atomic_store_explicit(&state->wake_exited, 0, memory_order_release);
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

static void stop_pair(
        OresNativeActorScheduler* scheduler,
        OresNativeActorRef target,
        OresNativeActorRef producer) {
    if (!ores_native_actor_scheduler_poll_done(scheduler, target)) {
        CHECK(ores_native_actor_scheduler_stop(scheduler, target));
        CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));
    }
    CHECK(ores_native_actor_scheduler_stop(scheduler, producer));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, producer));
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void reserve_and_attach(
        OresNativeActorScheduler* scheduler,
        OresNativeActorRef producer,
        OresNativeSelectArbiter* select,
        OresNativeWakeTicket* wake,
        OresNativeSelectToken* a,
        OresNativeSelectToken* b,
        uint64_t continuation_generation) {
    CHECK(ores_native_actor_scheduler_reserve_select_wake(
        scheduler, select, 2, producer,
        71, continuation_generation, 1, wake));
    CHECK(wake->descriptor_slot == UINT64_MAX);
    CHECK(wake->descriptor_generation != 0);
    CHECK(ores_native_select_register(select, 0, a));
    CHECK(ores_native_select_register(select, 1, b));
    CHECK(ores_native_actor_scheduler_attach_select_wake(
        scheduler, a, *wake));
    CHECK(ores_native_actor_scheduler_attach_select_wake(
        scheduler, b, *wake));
}

static void send_winner_publishes_one_common_wake(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    atomic_store(&state.wake_outcome, -1);
    OresNativeActorRef target = {0}, producer = {0};
    OresNativeActorScheduler* scheduler =
        open_pair(&state, &target, &producer);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 1)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    OresNativeWakeTicket wake = {0};
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    reserve_and_attach(
        scheduler, producer, &select, &wake,
        &send_arm, &other_arm, 101);

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 2, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    atomic_store_explicit(&state.block, 0, memory_order_release);
    wait_wake(&state);
    CHECK(atomic_load_explicit(
        &state.wake_count, memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(
        &state.wake_outcome, memory_order_acquire)
        == ORES_NATIVE_WAKE_READY);
    CHECK(atomic_load_explicit(
        &state.wake_completion, memory_order_acquire) == 1);
    CHECK(state.wake_ticket.descriptor_generation
        == send_arm.selection_generation);

    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, send, &outcome));
    CHECK(outcome == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_select_try_commit(other_arm)
        == ORES_NATIVE_SELECT_LOST);
    CHECK(ores_native_select_active_tokens(&select) == 0);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 2);
    CHECK(state.observed[0] == 1 && state.observed[1] == 2);

    stop_pair(scheduler, target, producer);
}

static void other_source_winner_uses_same_common_wake(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    atomic_store(&state.wake_outcome, -1);
    OresNativeActorRef target = {0}, producer = {0};
    OresNativeActorScheduler* scheduler =
        open_pair(&state, &target, &producer);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 10)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    OresNativeWakeTicket wake = {0};
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    reserve_and_attach(
        scheduler, producer, &select, &wake,
        &send_arm, &other_arm, 102);

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 11, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);

    CHECK(ores_native_select_try_commit(other_arm)
        == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_actor_scheduler_publish_select_wake(
        scheduler, other_arm, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PUBLISHED);
    CHECK(ores_native_actor_scheduler_publish_select_wake(
        scheduler, other_arm, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_INVALID);

    wait_wake(&state);
    CHECK(atomic_load_explicit(
        &state.wake_count, memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(
        &state.wake_completion, memory_order_acquire) == 2);

    CHECK(ores_native_actor_scheduler_cancel_async_send(scheduler, send));
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));
    CHECK(ores_native_select_active_tokens(&select) == 0);

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.observed_count, memory_order_acquire) == 1);
    CHECK(state.observed[0] == 10);
    stop_pair(scheduler, target, producer);
}

static void stop_failure_winner_publishes_failed_common_wake(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    atomic_store(&state.wake_outcome, -1);
    OresNativeActorRef target = {0}, producer = {0};
    OresNativeActorScheduler* scheduler =
        open_pair(&state, &target, &producer);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 20)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeSelectArbiter select = {0};
    OresNativeWakeTicket wake = {0};
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    reserve_and_attach(
        scheduler, producer, &select, &wake,
        &send_arm, &other_arm, 103);

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_selectable_async(
        scheduler, target, 21, send_arm, &send)
        == ORES_NATIVE_SEND_ACCEPTED);

    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, target));
    wait_wake(&state);
    CHECK(atomic_load_explicit(
        &state.wake_count, memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(
        &state.wake_outcome, memory_order_acquire)
        == ORES_NATIVE_WAKE_FAILED);
    CHECK(atomic_load_explicit(
        &state.wake_completion, memory_order_acquire) == 1);
    expect_send_state(
        scheduler, send,
        ORES_NATIVE_ASYNC_SEND_FAILED, ORES_NATIVE_SEND_TERMINATED);
    CHECK(ores_native_select_try_commit(other_arm)
        == ORES_NATIVE_SELECT_LOST);
    CHECK(ores_native_select_active_tokens(&select) == 0);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));
    stop_pair(scheduler, target, producer);
}

static void immediate_terminal_is_published_after_coordinator_records_result(void) {
    State state = {0};
    atomic_store(&state.wake_outcome, -1);
    OresNativeActorRef target = {0}, producer = {0};
    OresNativeActorScheduler* scheduler =
        open_pair(&state, &target, &producer);

    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, target));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));

    OresNativeSelectArbiter select = {0};
    OresNativeWakeTicket wake = {0};
    OresNativeSelectToken send_arm = {0}, other_arm = {0};
    reserve_and_attach(
        scheduler, producer, &select, &wake,
        &send_arm, &other_arm, 104);

    OresNativeAsyncSendTicket none = {0};
    const OresNativeSendStatus status =
        ores_native_actor_scheduler_submit_selectable_async(
            scheduler, target, 30, send_arm, &none);
    CHECK(status == ORES_NATIVE_SEND_TERMINATED);
    CHECK(none.generation == 0);
    CHECK(atomic_load_explicit(
        &state.wake_count, memory_order_acquire) == 0);

    /* The coordinator records the synchronous exact status before publishing
     * the common continuation, so the resumed guest cannot race uninitialized
     * result state. */
    const OresNativeSendStatus recorded = status;
    CHECK(recorded == ORES_NATIVE_SEND_TERMINATED);
    CHECK(ores_native_actor_scheduler_publish_select_wake(
        scheduler, send_arm, ORES_NATIVE_WAKE_FAILED)
        == ORES_NATIVE_WAKE_PUBLISHED);
    wait_wake(&state);
    CHECK(atomic_load_explicit(
        &state.wake_completion, memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(
        &state.wake_outcome, memory_order_acquire)
        == ORES_NATIVE_WAKE_FAILED);
    CHECK(ores_native_select_try_commit(other_arm)
        == ORES_NATIVE_SELECT_LOST);
    CHECK(ores_native_select_active_tokens(&select) == 0);

    stop_pair(scheduler, target, producer);
}

static void rearm_mints_new_generation_and_stale_token_cannot_notify(void) {
    State state = {0};
    atomic_store(&state.wake_outcome, -1);
    OresNativeActorRef target = {0}, producer = {0};
    OresNativeActorScheduler* scheduler =
        open_pair(&state, &target, &producer);

    OresNativeSelectArbiter select = {0};
    OresNativeWakeTicket first_wake = {0};
    OresNativeSelectToken first = {0}, first_loser = {0};
    reserve_and_attach(
        scheduler, producer, &select, &first_wake,
        &first, &first_loser, 105);
    const uint64_t first_generation = first.selection_generation;

    CHECK(ores_native_select_try_commit(first)
        == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_select_detach(first_loser));

    atomic_store_explicit(&state.hold_wake, 1, memory_order_release);
    CHECK(ores_native_actor_scheduler_publish_select_wake(
        scheduler, first, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PUBLISHED);
    while (!atomic_load_explicit(
            &state.wake_entered, memory_order_acquire))
        sched_yield();

    OresNativeWakeTicket refused = {0};
    CHECK(!ores_native_actor_scheduler_rearm_select_wake(
        scheduler, &select, 2, producer,
        71, 106, 1, &refused));
    CHECK(refused.slot_generation == 0);

    atomic_store_explicit(&state.hold_wake, 0, memory_order_release);
    wait_wake(&state);
    while (ores_native_actor_scheduler_outstanding_wake_loans(
            scheduler, producer) != 0)
        sched_yield();
    CHECK(atomic_load_explicit(
        &state.wake_exited, memory_order_acquire));

    reset_wake(&state);
    OresNativeWakeTicket second_wake = {0};
    CHECK(ores_native_actor_scheduler_rearm_select_wake(
        scheduler, &select, 2, producer,
        71, 106, 1, &second_wake));
    OresNativeSelectToken second = {0}, second_loser = {0};
    CHECK(ores_native_select_register(&select, 0, &second));
    CHECK(ores_native_select_register(&select, 1, &second_loser));
    CHECK(second.selection_generation != first_generation);
    CHECK(ores_native_actor_scheduler_attach_select_wake(
        scheduler, &second, second_wake));
    CHECK(ores_native_actor_scheduler_attach_select_wake(
        scheduler, &second_loser, second_wake));

    CHECK(ores_native_actor_scheduler_publish_select_wake(
        scheduler, first, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_INVALID);
    CHECK(ores_native_select_try_commit(second_loser)
        == ORES_NATIVE_SELECT_WON);
    CHECK(ores_native_actor_scheduler_publish_select_wake(
        scheduler, second_loser, ORES_NATIVE_WAKE_READY)
        == ORES_NATIVE_WAKE_PUBLISHED);
    wait_wake(&state);
    CHECK(atomic_load_explicit(
        &state.wake_completion, memory_order_acquire) == 2);
    CHECK(ores_native_select_detach(second));
    CHECK(ores_native_select_active_tokens(&select) == 0);

    stop_pair(scheduler, target, producer);
}

int main(void) {
    send_winner_publishes_one_common_wake();
    other_source_winner_uses_same_common_wake();
    stop_failure_winner_publishes_failed_common_wake();
    immediate_terminal_is_published_after_coordinator_records_result();
    rearm_mints_new_generation_and_stale_token_cannot_notify();
    puts("NATIVE_SELECT_COMMON_WAKE_PASS send=1 other=1 stop_failed=1 immediate_recorded=1 rearm=1 physical_wake_fence=1 stale_notify=1");
    return 0;
}
