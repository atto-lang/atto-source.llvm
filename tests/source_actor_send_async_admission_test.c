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
static void* carrier_probe_actor;
static atomic_int carrier_probe_status;

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
    CHECK(carrier_probe_actor != NULL);
    const int status = oreslang_source_actor_await_send_async_checked(
        carrier_probe_actor, payload + 1);
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
    while (!atomic_load_explicit(&waiter.started, memory_order_acquire))
        sched_yield();

    /* Capacity cannot change while the sole active turn is blocked. */
    for (int i = 0; i < 10000; ++i) {
        CHECK(!atomic_load_explicit(&waiter.done, memory_order_acquire));
        sched_yield();
    }

    atomic_store_explicit(&release_first, 1, memory_order_release);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(waiter.status == ORES_SOURCE_SEND_ASYNC_ACCEPTED);
    CHECK(atomic_load_explicit(&waiter.done, memory_order_acquire));

    oreslang_source_actor_stop(actor);
    fprintf(stderr,
        "SOURCE_SEND_ASYNC_CAPACITY_PASS queued=%zu exact_ref=1\n", queued);
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
    while (!atomic_load_explicit(&waiter.started, memory_order_acquire))
        sched_yield();
    for (int i = 0; i < 10000; ++i) {
        CHECK(!atomic_load_explicit(&waiter.done, memory_order_acquire));
        sched_yield();
    }

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

static void carrier_call_fails_would_deadlock(void) {
    carrier_probe_actor = NULL;
    atomic_store_explicit(&carrier_probe_status, -1, memory_order_release);
    void* actor = oreslang_source_actor_spawn(
        init_actor, carrier_probe_message, 1);
    CHECK(actor != NULL);
    wait_ready(actor);
    carrier_probe_actor = actor;

    CHECK(oreslang_source_actor_try_send_checked(actor, 5)
        == ORES_NATIVE_SEND_ACCEPTED);
    while (atomic_load_explicit(
            &carrier_probe_status, memory_order_acquire) == -1)
        sched_yield();

    CHECK(atomic_load_explicit(
        &carrier_probe_status, memory_order_acquire)
        == ORES_SOURCE_SEND_ASYNC_WOULD_DEADLOCK);
    oreslang_source_actor_stop(actor);
    carrier_probe_actor = NULL;
    fprintf(stderr, "SOURCE_SEND_ASYNC_DEADLOCK_PASS carrier_rejected=1\n");
}

int main(void) {
    atomic_init(&first_entered, 0);
    atomic_init(&release_first, 0);
    atomic_init(&block_first, 0);
    atomic_init(&carrier_probe_status, -1);

    capacity_release_admits_waiter();
    stop_wakes_full_waiter_as_terminated();
    carrier_call_fails_would_deadlock();
    return 0;
}
