#include "oreslang/runtime/SourceActorBootstrap.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) abort(); } while (0)

static atomic_int first_entered;
static atomic_int release_first;
static atomic_int block_first;
static atomic_uintptr_t carrier_probe_actor;
static atomic_int carrier_probe_status;
/* This foreign scheduler has a distinct domain from SourceActor's scheduler.
 * The carrier-side wait guard must not depend on scheduler pointer equality. */
static OresNativeActorScheduler* foreign_scheduler;
static OresNativeActorRef foreign_ref;
static atomic_int foreign_probe_status;

static void foreign_noop(void* ctx, size_t slot, uint64_t payload) {
    (void)ctx; (void)slot; (void)payload;
}

static int64_t foreign_wait_probe_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    CHECK(foreign_scheduler != NULL);
    const OresNativeSendStatus status = ores_native_actor_scheduler_wait_send(
        foreign_scheduler, foreign_ref, (uint64_t)payload);
    atomic_store_explicit(
        &foreign_probe_status, (int)status, memory_order_release);
    return payload;
}

static void init_actor(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap; (void)domain; (void)epoch;
}

static int64_t blocking_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    if (atomic_exchange_explicit(&block_first, 0, memory_order_acq_rel)) {
        atomic_store_explicit(&first_entered, 1, memory_order_release);
        while (!atomic_load_explicit(&release_first, memory_order_acquire))
            sched_yield();
    }
    return payload;
}

static int64_t carrier_probe_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    void* actor = (void*)atomic_load_explicit(
        &carrier_probe_actor, memory_order_acquire);
    CHECK(actor != NULL);
    const int status = oreslang_source_actor_await_send_async_checked(
        actor, payload + 1);
    atomic_store_explicit(
        &carrier_probe_status, status, memory_order_release);
    return payload;
}

typedef struct {
    void* actor;
    atomic_int started;
    atomic_int done;
    int status;
} AwaitThread;

static void* await_send(void* raw) {
    AwaitThread* state = (AwaitThread*)raw;
    atomic_store_explicit(&state->started, 1, memory_order_release);
    state->status =
        oreslang_source_actor_await_send_async_checked(state->actor, 999);
    atomic_store_explicit(&state->done, 1, memory_order_release);
    return NULL;
}

static int same_ref(OresNativeActorRef a, OresNativeActorRef b) {
    return a.scheduler_domain != 0
        && a.scheduler_domain == b.scheduler_domain
        && a.actor_slot == b.actor_slot
        && a.generation == b.generation;
}

static void reset_blocker(void) {
    atomic_store_explicit(&first_entered, 0, memory_order_release);
    atomic_store_explicit(&release_first, 0, memory_order_release);
    atomic_store_explicit(&block_first, 1, memory_order_release);
}

static void wait_ready(void* actor) {
    for (;;) {
        const int status = oreslang_source_actor_ready_status(actor);
        if (status == ORES_SOURCE_ACTOR_STARTUP_READY) return;
        CHECK(status == ORES_SOURCE_ACTOR_STARTUP_PENDING
           || status == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING);
        sched_yield();
    }
}

static size_t saturate_real_mailbox(void* actor) {
    CHECK(oreslang_source_actor_try_send_checked(actor, 1)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (!atomic_load_explicit(&first_entered, memory_order_acquire))
        sched_yield();

    size_t queued = 0;
    for (; queued < 1024; ++queued) {
        const int status =
            oreslang_source_actor_try_send_checked(actor, (int64_t)(queued + 2));
        if (status == ORES_NATIVE_SEND_MAILBOX_FULL) break;
        CHECK(status == ORES_NATIVE_SEND_ACCEPTED);
    }
    CHECK(queued > 0 && queued < 1024);
    CHECK(oreslang_source_actor_try_send_checked(actor, 777)
        == ORES_NATIVE_SEND_MAILBOX_FULL);
    return queued;
}

static void capacity_release_admits_waiter(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    OresNativeActorRef ref_a = {0}, ref_b = {0};
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &ref_a));
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &ref_b));
    CHECK(same_ref(ref_a, ref_b));

    const size_t queued = saturate_real_mailbox(actor);
    AwaitThread waiter = {.actor = actor, .status = -1};
    atomic_init(&waiter.started, 0);
    atomic_init(&waiter.done, 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, await_send, &waiter) == 0);
    while (oreslang_source_actor_test_pending_async_sends(actor) != 1)
        sched_yield();

    /* The scheduler-mutex snapshot proves a scheduler-owned Future is
     * genuinely PENDING while FULL. The blocked handler prevents capacity
     * from changing; no producer thread is required for Future progress. */
    CHECK(!atomic_load_explicit(&waiter.done, memory_order_acquire));

    atomic_store_explicit(&release_first, 1, memory_order_release);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.status == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(atomic_load_explicit(&waiter.done, memory_order_acquire));

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_CAPACITY_PASS queued=%zu exact_ref=1\n", queued);
}

static void stored_future_starts_before_await_and_progresses(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 901, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);

    /* Submission has returned to the caller while target admission remains
     * pending. Releasing target capacity progresses the Future independently
     * of any source await. */
    atomic_store_explicit(&release_first, 1, memory_order_release);
    while (oreslang_source_actor_test_pending_async_sends(actor) != 0)
        sched_yield();

    CHECK(oreslang_source_actor_await_send_future_checked(future)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);
    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_FUTURE_PASS eager_start=1 independent_progress=1\n");
}

static void stale_source_future_pointer_never_aliases_new_future(void) {
    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    void* first = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 911, &first) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(first != NULL);
    CHECK(oreslang_source_actor_await_send_future_checked(first)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    void* second = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 912, &second) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(second != NULL);
    CHECK(second != first);
    CHECK(oreslang_source_actor_await_send_future_checked(first)
        == ORES_SOURCE_SEND_ASYNC_INVALID_REF);
    CHECK(oreslang_source_actor_await_send_future_checked(second)
        == ORES_SOURCE_SEND_ASYNC_ACCEPTED);

    oreslang_source_actor_stop(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_STALE_HANDLE_PASS nonrecycled=1\n");
}

static void stored_future_cancel_wins_only_while_pending(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    void* future = NULL;
    CHECK(oreslang_source_actor_send_async_start_checked(
        actor, 902, &future) == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(future != NULL);
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 1);
    CHECK(oreslang_source_actor_cancel_send_future(future));
    CHECK(oreslang_source_actor_test_pending_async_sends(actor) == 0);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    oreslang_source_actor_stop(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_CANCEL_PASS pending_only=1\n");
}

static void stop_wakes_full_waiter_as_terminated(void) {
    reset_blocker();
    void* actor = oreslang_source_actor_spawn(
        init_actor, blocking_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    (void)saturate_real_mailbox(actor);

    AwaitThread waiter = {.actor = actor, .status = -1};
    atomic_init(&waiter.started, 0);
    atomic_init(&waiter.done, 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, await_send, &waiter) == 0);
    while (oreslang_source_actor_test_pending_async_sends(actor) != 1)
        sched_yield();
    CHECK(!atomic_load_explicit(&waiter.done, memory_order_acquire));

    /* Sealed STOP is itself a scheduler credit-state transition. The waiter
     * must wake and observe TERMINATED even though the active turn is still
     * physically blocked and cannot return ordinary mailbox capacity yet. */
    oreslang_source_actor_request_stop(actor);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.status == ORES_SOURCE_SEND_ASYNC_TERMINATED);

    atomic_store_explicit(&release_first, 1, memory_order_release);
    oreslang_source_actor_join_done(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_STOP_PASS terminated=1\n");
}

static int64_t passthrough_message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap; (void)domain; (void)epoch;
    return payload;
}

static void bounded_waiter_cohort_rejects_before_pin(void) {
    void* actor = oreslang_source_actor_spawn(
        init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);

    CHECK(oreslang_source_actor_test_set_async_waiters(
        actor, ORES_SOURCE_SEND_ASYNC_MAX_WAITERS));
    CHECK(oreslang_source_actor_await_send_async_checked(actor, 7)
        == ORES_SOURCE_SEND_ASYNC_RESOURCE_EXHAUSTED);
    CHECK(oreslang_source_actor_test_set_async_waiters(actor, 0));

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_COHORT_PASS max_waiters=%d resource_exhausted=1\n",
        ORES_SOURCE_SEND_ASYNC_MAX_WAITERS);
}

static void carrier_call_fails_would_deadlock(void) {
    atomic_store_explicit(&carrier_probe_actor, (uintptr_t)0, memory_order_release);
    atomic_store_explicit(&carrier_probe_status, -1, memory_order_release);
    void* actor = oreslang_source_actor_spawn(
        init_actor, carrier_probe_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    atomic_store_explicit(
        &carrier_probe_actor, (uintptr_t)actor, memory_order_release);

    CHECK(oreslang_source_actor_try_send_checked(actor, 5)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (atomic_load_explicit(
            &carrier_probe_status, memory_order_acquire) == -1)
        sched_yield();

    CHECK(atomic_load_explicit(
        &carrier_probe_status, memory_order_acquire)
        == ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK);
    oreslang_source_actor_stop(actor);
    atomic_store_explicit(
        &carrier_probe_actor, (uintptr_t)0, memory_order_release);
    fprintf(stderr, "SOURCE_SEND_ASYNC_DEADLOCK_PASS carrier_rejected=1\n");
}

static void foreign_carrier_probe(void* raw, size_t slot, uint64_t payload) {
    (void)slot;
    CHECK(ores_native_actor_scheduler_is_carrier_thread());
    const int status = oreslang_source_actor_await_send_async_checked(
        raw, (int64_t)payload);
    atomic_store_explicit(&carrier_probe_status, status, memory_order_release);
}

static void foreign_scheduler_carrier_fails_would_deadlock(void) {
    void* actor = oreslang_source_actor_spawn(init_actor, passthrough_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    atomic_store_explicit(&carrier_probe_status, -1, memory_order_release);
    OresNativeActorScheduler* foreign = ores_native_actor_scheduler_open(
        1, 1, 1, foreign_carrier_probe, actor);
    CHECK(foreign);
    OresNativeActorRef producer = ores_native_actor_scheduler_spawn(foreign, 0);
    CHECK(ores_native_actor_scheduler_send(foreign, producer, 17));
    CHECK(ores_native_actor_scheduler_drain(foreign));
    CHECK(atomic_load_explicit(&carrier_probe_status, memory_order_acquire)
        == ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK);
    CHECK(!ores_native_actor_scheduler_is_carrier_thread());
    CHECK(ores_native_actor_scheduler_stop(foreign, producer));
    ores_native_actor_scheduler_retire(foreign);
    CHECK(ores_native_actor_scheduler_try_destroy(foreign));
    oreslang_source_actor_stop(actor);
    fprintf(stderr, "SOURCE_SEND_ASYNC_FOREIGN_CARRIER_PASS rejected=1\n");
}

static void cross_scheduler_carrier_cannot_park(void) {
    atomic_store_explicit(&foreign_probe_status, -1, memory_order_release);
    foreign_scheduler = ores_native_actor_scheduler_open(
        1, 1, 1, foreign_noop, NULL);
    CHECK(foreign_scheduler != NULL);
    foreign_ref = ores_native_actor_scheduler_spawn(foreign_scheduler, 0);
    CHECK(foreign_ref.scheduler_domain != 0);

    void* actor = oreslang_source_actor_spawn(
        init_actor, foreign_wait_probe_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    OresNativeActorRef source_ref = {0};
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &source_ref));
    CHECK(source_ref.scheduler_domain != foreign_ref.scheduler_domain);

    CHECK(oreslang_source_actor_try_send_checked(actor, 83)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (atomic_load_explicit(
            &foreign_probe_status, memory_order_acquire) == -1)
        sched_yield();
    /* Previously executing != foreign_scheduler permitted blocking here.
     * The API is external-host-only even when the target has free slots. */
    CHECK(atomic_load_explicit(
        &foreign_probe_status, memory_order_acquire)
        == ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE);
    oreslang_source_actor_stop(actor);

    CHECK(ores_native_actor_scheduler_stop(
        foreign_scheduler, foreign_ref));
    CHECK(ores_native_actor_scheduler_wait_done(
        foreign_scheduler, foreign_ref));
    ores_native_actor_scheduler_retire(foreign_scheduler);
    CHECK(ores_native_actor_scheduler_try_destroy(foreign_scheduler));
    foreign_scheduler = NULL;
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_CROSS_SCHEDULER_DEADLOCK_PASS rejected=1\n");
}

int main(void) {
    atomic_init(&first_entered, 0);
    atomic_init(&release_first, 0);
    atomic_init(&block_first, 0);
    atomic_init(&carrier_probe_actor, (uintptr_t)0);
    atomic_init(&carrier_probe_status, -1);
    atomic_init(&foreign_probe_status, -1);

    capacity_release_admits_waiter();
    stored_future_starts_before_await_and_progresses();
    stale_source_future_pointer_never_aliases_new_future();
    stored_future_cancel_wins_only_while_pending();
    stop_wakes_full_waiter_as_terminated();
    bounded_waiter_cohort_rejects_before_pin();
    carrier_call_fails_would_deadlock();
    foreign_scheduler_carrier_fails_would_deadlock();
    cross_scheduler_carrier_cannot_park();
    return 0;
}
