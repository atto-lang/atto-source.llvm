#include "oreslang/runtime/SourceActorBootstrap.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_int entered;
static atomic_int release_handler;
static atomic_int exited;
static atomic_int delivered;

static void check(int ok) {
    if (!ok) abort();
}

static void actor_init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain != 0 && epoch != 0);
}

static int64_t actor_message(
        OresActorI64Heap* heap,
        uint64_t domain,
        uint64_t epoch,
        int64_t payload) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    atomic_store_explicit(&entered, 1, memory_order_release);
    while (!atomic_load_explicit(&release_handler, memory_order_acquire))
        sched_yield();
    atomic_fetch_add_explicit(&delivered, 1, memory_order_acq_rel);
    atomic_store_explicit(&exited, 1, memory_order_release);
    return payload;
}

int main(void) {
    void* actor = oreslang_source_actor_spawn(actor_init, actor_message, 1);
    check(actor != NULL);
    oreslang_source_actor_send(actor, 7);
    oreslang_source_actor_send(actor, 8);

    while (!atomic_load_explicit(&entered, memory_order_acquire))
        sched_yield();

    /*
     * Message 7 owns the active turn and message 8 is queued behind it.
     * Source stop must return without waiting, preserve the already-active
     * turn, and abandon the queued DATA envelope. If request_stop waits for
     * DONE this deadlocks; if it drains queued DATA delivered becomes 2.
     */
    oreslang_source_actor_request_stop(actor);
    check(!atomic_load_explicit(&exited, memory_order_acquire));

    atomic_store_explicit(&release_handler, 1, memory_order_release);
    oreslang_source_actor_join_done(actor);
    check(atomic_load_explicit(&exited, memory_order_acquire));
    check(atomic_load_explicit(&delivered, memory_order_acquire) == 1);

    puts("SOURCE_ACTOR_STOP_JOIN_SPLIT_PASS");
    return 0;
}
