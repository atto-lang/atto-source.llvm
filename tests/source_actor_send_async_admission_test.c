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
static atomic_int block_first = 1;

static void init_actor(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap; (void)domain; (void)epoch;
}

static int64_t on_message(
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

int main(void) {
    atomic_init(&first_entered, 0);
    atomic_init(&release_first, 0);

    void* actor = oreslang_source_actor_spawn(init_actor, on_message, 1);
    CHECK(actor != NULL);

    while (oreslang_source_actor_ready_status(actor)
            == ORES_SOURCE_ACTOR_STARTUP_PENDING
        || oreslang_source_actor_ready_status(actor)
            == ORES_SOURCE_ACTOR_STARTUP_INITIALIZING)
        sched_yield();
    CHECK(oreslang_source_actor_ready_status(actor)
        == ORES_SOURCE_ACTOR_STARTUP_READY);

    OresNativeActorRef ref_a = {0}, ref_b = {0};
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &ref_a));
    CHECK(oreslang_source_actor_exact_scheduler_ref(actor, &ref_b));
    CHECK(same_ref(ref_a, ref_b));

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

    AwaitThread waiter = {.actor = actor, .status = -1};
    atomic_init(&waiter.started, 0);
    atomic_init(&waiter.done, 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, await_send, &waiter) == 0);
    while (!atomic_load_explicit(&waiter.started, memory_order_acquire))
        sched_yield();

    /* With the only logical actor turn blocked and its queue full, admission
     * cannot complete before a physical dequeue returns credit. */
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
        "SOURCE_SEND_ASYNC_ADMISSION_PASS queued=%zu exact_ref=1\n", queued);
    return 0;
}
