#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) abort(); } while (0)

enum { MAX_OBSERVED = 1024 };

typedef struct {
    atomic_int block;
    atomic_int entered;
    atomic_uint observed_count;
    uint64_t observed[MAX_OBSERVED];
} State;

static void handler(void* raw, size_t actor, uint64_t payload) {
    State* state = (State*)raw;
    CHECK(actor == 0);
    unsigned index = atomic_fetch_add_explicit(
        &state->observed_count, 1, memory_order_acq_rel);
    CHECK(index < MAX_OBSERVED);
    state->observed[index] = payload;
    atomic_store_explicit(&state->entered, 1, memory_order_release);
    while (atomic_load_explicit(&state->block, memory_order_acquire))
        sched_yield();
}

static void wait_entered(State* state) {
    while (!atomic_load_explicit(&state->entered, memory_order_acquire))
        sched_yield();
}

static void expect_state(
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

static OresNativeActorScheduler* open_one(
        State* state, size_t capacity, OresNativeActorRef* actor) {
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open(1, capacity, 1, handler, state);
    CHECK(scheduler != NULL);
    *actor = ores_native_actor_scheduler_spawn(scheduler, 0);
    CHECK(actor->scheduler_domain != 0);
    return scheduler;
}

static void close_one(
        OresNativeActorScheduler* scheduler, OresNativeActorRef actor) {
    CHECK(ores_native_actor_scheduler_stop(scheduler, actor));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, actor));
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void immediate_resource_failure_does_not_mint_future(void) {
    State state = {0};
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 2, &actor);

    CHECK(ores_native_actor_scheduler_started_carriers(scheduler) == 0);
    ores_native_actor_scheduler_test_fail_next_create();

    OresNativeAsyncSendTicket failed = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 9, &failed)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    CHECK(failed.generation == 0);
    CHECK(failed.target.scheduler_domain == 0);
    CHECK(ores_native_actor_scheduler_started_carriers(scheduler) == 0);

    /* The failed immediate admission consumed no descriptor. A subsequent
     * healthy submission may use the cohort normally and becomes READY. */
    OresNativeAsyncSendTicket ready = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 10, &ready) == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ready.generation != 0);
    expect_state(
        scheduler, ready,
        ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, ready));

    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load(&state.observed_count) == 1);
    CHECK(state.observed[0] == 10);
    close_one(scheduler, actor);
}

static void immediate_future_is_ready_before_await(void) {
    State state = {0};
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 2, &actor);

    OresNativeAsyncSendTicket ticket = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 11, &ticket) == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ticket.generation != 0);
    expect_state(
        scheduler, ticket,
        ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);

    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, ticket, &outcome));
    CHECK(outcome == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, ticket));

    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load(&state.observed_count) == 1);
    CHECK(state.observed[0] == 11);
    close_one(scheduler, actor);
}

static void fifo_future_owns_released_credit_before_sync_sender(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 1, &actor);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, actor, 100)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeAsyncSendTicket first = {0}, second = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 1, &first) == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 2, &second) == ORES_NATIVE_SEND_ACCEPTED);
    expect_state(
        scheduler, first,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);
    expect_state(
        scheduler, second,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, actor, 99)
        == ORES_NATIVE_SEND_MAILBOX_FULL);

    atomic_store_explicit(&state.block, 0, memory_order_release);

    OresNativeSendStatus first_outcome = ORES_NATIVE_SEND_INVALID_REF;
    OresNativeSendStatus second_outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, first, &first_outcome));
    CHECK(first_outcome == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, second, &second_outcome));
    CHECK(second_outcome == ORES_NATIVE_SEND_ACCEPTED);

    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, first));
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, second));
    CHECK(ores_native_actor_scheduler_drain(scheduler));

    CHECK(atomic_load(&state.observed_count) == 3);
    CHECK(state.observed[0] == 100);
    CHECK(state.observed[1] == 1);
    CHECK(state.observed[2] == 2);
    close_one(scheduler, actor);
}

static void cancel_pending_never_delivers_payload(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 1, &actor);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, actor, 200)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeAsyncSendTicket ticket = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 7, &ticket) == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_cancel_async_send(scheduler, ticket));
    expect_state(
        scheduler, ticket,
        ORES_NATIVE_ASYNC_SEND_CANCELLED,
        ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, ticket));

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load(&state.observed_count) == 1);
    CHECK(state.observed[0] == 200);
    close_one(scheduler, actor);
}

static void stop_fails_pending_but_not_ready_future(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 1, &actor);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, actor, 300)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeAsyncSendTicket pending = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 8, &pending) == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, actor));
    expect_state(
        scheduler, pending,
        ORES_NATIVE_ASYNC_SEND_FAILED, ORES_NATIVE_SEND_TERMINATED);
    CHECK(!ores_native_actor_scheduler_cancel_async_send(scheduler, pending));
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, pending));

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, actor));
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void descriptor_bound_and_generation_fence(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 1, &actor);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, actor, 400)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeAsyncSendTicket tickets[ORES_NATIVE_ASYNC_SENDS_PER_ACTOR];
    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        tickets[i] = (OresNativeAsyncSendTicket){0};
        CHECK(ores_native_actor_scheduler_submit_async(
            scheduler, actor, 1000 + i, &tickets[i])
            == ORES_NATIVE_SEND_ACCEPTED);
        expect_state(
            scheduler, tickets[i],
            ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);
    }

    OresNativeAsyncSendTicket overflow = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 9999, &overflow)
        == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    CHECK(overflow.generation == 0);

    const OresNativeAsyncSendTicket stale = tickets[0];
    CHECK(ores_native_actor_scheduler_cancel_async_send(
        scheduler, tickets[0]));
    CHECK(ores_native_actor_scheduler_release_async_send(
        scheduler, tickets[0]));

    OresNativeAsyncSendTicket reused = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 5000, &reused) == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(reused.slot == stale.slot);
    CHECK(reused.generation != stale.generation);

    OresNativeAsyncSendState old_state = ORES_NATIVE_ASYNC_SEND_INVALID;
    OresNativeSendStatus old_outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(!ores_native_actor_scheduler_async_send_status(
        scheduler, stale, &old_state, &old_outcome));

    CHECK(ores_native_actor_scheduler_cancel_async_send(scheduler, reused));
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, reused));
    for (size_t i = 1; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        CHECK(ores_native_actor_scheduler_cancel_async_send(
            scheduler, tickets[i]));
        CHECK(ores_native_actor_scheduler_release_async_send(
            scheduler, tickets[i]));
    }

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    close_one(scheduler, actor);
}

/* Admission, not eventual handler execution, settles a READY send Future.
 * STOP may abandon queued USER data without retroactively revoking the
 * already-issued mailbox-admission receipt. */
static void sealed_stop_preserves_ready_admission(void) {
    State state = {0};
    atomic_store(&state.block, 1);
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 2, &actor);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, actor, 601)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state);

    OresNativeAsyncSendTicket ready = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 602, &ready) == ORES_NATIVE_SEND_ACCEPTED);
    expect_state(scheduler, ready,
                 ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);

    /* The active first callback cannot exit yet. The second accepted
     * message is queued and will be abandoned by sealed STOP. */
    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, actor));
    expect_state(scheduler, ready,
                 ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);
    CHECK(!ores_native_actor_scheduler_cancel_async_send(scheduler, ready));
    OresNativeSendStatus outcome = ORES_NATIVE_SEND_INVALID_REF;
    CHECK(ores_native_actor_scheduler_await_async_send(
        scheduler, ready, &outcome));
    CHECK(outcome == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, ready));
    OresNativeAsyncSendState stale_state = ORES_NATIVE_ASYNC_SEND_INVALID;
    CHECK(!ores_native_actor_scheduler_async_send_status(
        scheduler, ready, &stale_state, &outcome));

    atomic_store_explicit(&state.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, actor));
    CHECK(atomic_load(&state.observed_count) == 1);
    CHECK(state.observed[0] == 601);
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

/* Lifecycle barriers must be external-host-only across scheduler domains.
 * A foreign carrier may otherwise block its pool waiting for another pool,
 * or join a scheduler during an actor turn. Use a QUIESCENT retired target
 * so the old same-scheduler-only guard fails deterministically, without a
 * test deadlock or thread scheduling timeout. */
typedef struct {
    OresNativeActorScheduler* retired_target;
    OresNativeActorRef stopped_actor;
    atomic_int observed;
} CrossSchedulerLifecycleProbe;

static void foreign_lifecycle_callback(
        void* raw, size_t slot, uint64_t payload) {
    CrossSchedulerLifecycleProbe* probe = (CrossSchedulerLifecycleProbe*)raw;
    CHECK(slot == 0 && payload == 41);
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    CHECK(!ores_native_actor_scheduler_wait_done(
        probe->retired_target, probe->stopped_actor));
    CHECK(!ores_native_actor_scheduler_drain(probe->retired_target));
    CHECK(!ores_native_actor_scheduler_try_destroy(probe->retired_target));
    atomic_store_explicit(&probe->observed, 1, memory_order_release);
}

static void cross_scheduler_carrier_rejects_host_lifecycle_barriers(void) {
    State state = {0};
    OresNativeActorRef stopped = {0};
    OresNativeActorScheduler* target = open_one(&state, 1, &stopped);

    CHECK(ores_native_actor_scheduler_stop(target, stopped));
    CHECK(ores_native_actor_scheduler_wait_done(target, stopped));
    ores_native_actor_scheduler_retire(target);

    CrossSchedulerLifecycleProbe probe = {
        .retired_target = target, .stopped_actor = stopped
    };
    atomic_init(&probe.observed, 0);
    OresNativeActorScheduler* foreign = ores_native_actor_scheduler_open(
        1, 1, 1, foreign_lifecycle_callback, &probe);
    CHECK(foreign != NULL);
    OresNativeActorRef carrier = ores_native_actor_scheduler_spawn(foreign, 0);
    CHECK(carrier.scheduler_domain != 0);
    CHECK(carrier.scheduler_domain != stopped.scheduler_domain);
    CHECK(ores_native_actor_scheduler_try_send(foreign, carrier, 41)
        == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_drain(foreign));
    CHECK(atomic_load_explicit(&probe.observed, memory_order_acquire) == 1);

    /* Only the original external host may destroy the quiescent target. */
    CHECK(ores_native_actor_scheduler_try_destroy(target));
    close_one(foreign, carrier);
}

static void live_future_fences_scheduler_destruction(void) {
    State state = {0};
    OresNativeActorRef actor = {0};
    OresNativeActorScheduler* scheduler = open_one(&state, 2, &actor);

    OresNativeAsyncSendTicket ticket = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, actor, 77, &ticket) == ORES_NATIVE_SEND_ACCEPTED);
    expect_state(
        scheduler, ticket,
        ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);

    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(ores_native_actor_scheduler_stop(scheduler, actor));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, actor));
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(!ores_native_actor_scheduler_try_destroy(scheduler));
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, ticket));
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

typedef struct {
    State target;
    OresNativeActorScheduler* scheduler;
    atomic_int wake_seen;
    atomic_int wake_outcome;
    OresNativeWakeTicket wake_ticket;
} BoundWakeState;

static void bound_wake_callback(
        void* raw, size_t actor, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    BoundWakeState* state = (BoundWakeState*)raw;
    CHECK(actor == 1);
    CHECK(ores_native_actor_scheduler_is_current_wake(
        state->scheduler, ticket));
    state->wake_ticket = ticket;
    atomic_store_explicit(
        &state->wake_outcome, (int)outcome, memory_order_release);
    atomic_store_explicit(&state->wake_seen, 1, memory_order_release);
}

static void ready_future_wake_survives_target_stop(void) {
    BoundWakeState state = {0};
    atomic_store(&state.target.block, 1);
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open_with_wakes(
            2, 2, 2, handler, NULL, NULL, bound_wake_callback, &state);
    CHECK(scheduler != NULL);
    state.scheduler = scheduler;
    OresNativeActorRef target =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    OresNativeActorRef producer =
        ores_native_actor_scheduler_spawn(scheduler, 1);
    CHECK(target.scheduler_domain && producer.scheduler_domain);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 701)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state.target);

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, target, 702, &send) == ORES_NATIVE_SEND_ACCEPTED);
    expect_state(scheduler, send,
        ORES_NATIVE_ASYNC_SEND_READY, ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, target));

    OresNativeWakeTicket wake = {0};
    CHECK(ores_native_actor_scheduler_bind_async_send_wake(
        scheduler, send, producer, 17, 23, 1, &wake));
    while (!atomic_load_explicit(&state.wake_seen, memory_order_acquire))
        sched_yield();
    CHECK(ores_native_actor_scheduler_drain(scheduler));
    CHECK(atomic_load_explicit(
        &state.wake_outcome, memory_order_acquire) == ORES_NATIVE_WAKE_READY);
    CHECK(state.wake_ticket.descriptor_slot == send.slot);
    CHECK(state.wake_ticket.descriptor_generation == send.generation);
    CHECK(state.wake_ticket.completion_code == ORES_NATIVE_SEND_ACCEPTED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));

    atomic_store_explicit(&state.target.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));
    CHECK(ores_native_actor_scheduler_stop(scheduler, producer));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, producer));
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

static void pending_stop_publishes_exact_failed_wake(void) {
    BoundWakeState state = {0};
    atomic_store(&state.target.block, 1);
    OresNativeActorScheduler* scheduler =
        ores_native_actor_scheduler_open_with_wakes(
            2, 1, 2, handler, NULL, NULL, bound_wake_callback, &state);
    CHECK(scheduler != NULL);
    state.scheduler = scheduler;
    OresNativeActorRef target =
        ores_native_actor_scheduler_spawn(scheduler, 0);
    OresNativeActorRef producer =
        ores_native_actor_scheduler_spawn(scheduler, 1);
    CHECK(target.scheduler_domain && producer.scheduler_domain);

    CHECK(ores_native_actor_scheduler_try_send(scheduler, target, 711)
        == ORES_NATIVE_SEND_ACCEPTED);
    wait_entered(&state.target);

    OresNativeAsyncSendTicket send = {0};
    CHECK(ores_native_actor_scheduler_submit_async(
        scheduler, target, 712, &send) == ORES_NATIVE_SEND_ACCEPTED);
    expect_state(scheduler, send,
        ORES_NATIVE_ASYNC_SEND_PENDING, ORES_NATIVE_SEND_MAILBOX_FULL);

    OresNativeWakeTicket wake = {0};
    CHECK(ores_native_actor_scheduler_bind_async_send_wake(
        scheduler, send, producer, 31, 37, 1, &wake));
    CHECK(!ores_native_actor_scheduler_release_async_send(scheduler, send));

    CHECK(ores_native_actor_scheduler_stop_sealed(scheduler, target));
    while (!atomic_load_explicit(&state.wake_seen, memory_order_acquire))
        sched_yield();
    while (ores_native_actor_scheduler_wakes_completed(scheduler) == 0)
        sched_yield();

    CHECK(atomic_load_explicit(
        &state.wake_outcome, memory_order_acquire) == ORES_NATIVE_WAKE_FAILED);
    CHECK(state.wake_ticket.completion_code == ORES_NATIVE_SEND_TERMINATED);
    expect_state(scheduler, send,
        ORES_NATIVE_ASYNC_SEND_FAILED, ORES_NATIVE_SEND_TERMINATED);
    CHECK(ores_native_actor_scheduler_release_async_send(scheduler, send));

    atomic_store_explicit(&state.target.block, 0, memory_order_release);
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, target));
    CHECK(ores_native_actor_scheduler_stop(scheduler, producer));
    CHECK(ores_native_actor_scheduler_wait_done(scheduler, producer));
    ores_native_actor_scheduler_retire(scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(scheduler));
}

int main(void) {
    immediate_resource_failure_does_not_mint_future();
    immediate_future_is_ready_before_await();
    fifo_future_owns_released_credit_before_sync_sender();
    cancel_pending_never_delivers_payload();
    stop_fails_pending_but_not_ready_future();
    sealed_stop_preserves_ready_admission();
    descriptor_bound_and_generation_fence();
    live_future_fences_scheduler_destruction();
    cross_scheduler_carrier_rejects_host_lifecycle_barriers();
    ready_future_wake_survives_target_stop();
    pending_stop_publishes_exact_failed_wake();
    puts("NATIVE_ASYNC_SEND_FUTURE_PASS fail_fast_resource=1 immediate=1 fifo=1 cancel=1 stop=1 ready_survives_stop=1 bound=256 aba=1 destroy_fence=1 wake_after_stop=1 exact_stop_wake=1 release_fence=1");
    return 0;
}
