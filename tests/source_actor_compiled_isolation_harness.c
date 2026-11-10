#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>

/* Trusted observation adapter around UNMODIFIED compiler-generated callbacks.
 * These probes are not guest APIs. No pointer is sent through a mailbox. */
extern void oreslang_actor_Worker_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Worker_on_message(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);

static atomic_int allocated;
static atomic_int ready;
static atomic_int deliveries[3];
static atomic_uintptr_t addresses[3];
static atomic_uint_fast64_t domains[3];
static atomic_int_fast64_t results[3];

static void check(int condition) { if (!condition) abort(); }

static void wait_for(atomic_int* counter, int count) {
    struct timespec start, now;
    check(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (atomic_load_explicit(counter, memory_order_acquire) != count) {
        check(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        check(now.tv_sec - start.tv_sec < 10);
        sched_yield();
    }
}

static int index_for(uint64_t domain) {
    for (int i = 0; i < 3; ++i)
        if (atomic_load_explicit(&domains[i], memory_order_acquire) == domain)
            return i;
    abort();
}

static void initialize(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    const int index = atomic_fetch_add_explicit(&allocated, 1, memory_order_relaxed);
    check(index >= 0 && index < 3);
    int64_t* first = NULL;
    int64_t* last = NULL;
    check(oreslang_source_actor_frame_i64_slot_checked(domain, epoch, 0, &first));
    check(oreslang_source_actor_frame_i64_slot_checked(domain, epoch, 8191, &last));
    check((uintptr_t)last - (uintptr_t)first == 65528);
    check(*first == 0 && *last == 0);
    int64_t* denied = first;
    check(!oreslang_source_actor_frame_i64_slot_checked(domain, epoch, 8192, &denied));
    check(denied == NULL);
    atomic_store_explicit(&addresses[index], (uintptr_t)first, memory_order_release);
    atomic_store_explicit(&domains[index], domain, memory_order_release);
    oreslang_actor_Worker_init(heap, domain, epoch);
    atomic_fetch_add_explicit(&ready, 1, memory_order_release);
}

static int64_t message(OresActorI64Heap* heap, uint64_t domain,
                       uint64_t epoch, int64_t input) {
    const int index = index_for(domain);
    int64_t* slot = NULL;
    check(oreslang_source_actor_frame_i64_slot_checked(domain, epoch, 0, &slot));
    check(*slot == 0); /* prior physical callback was scrubbed */
    int64_t* denied = slot;
    const uint64_t other = atomic_load_explicit(&domains[index == 0 ? 1 : 0], memory_order_acquire);
    check(other && other != domain);
    check(!oreslang_source_actor_frame_i64_slot_checked(other, epoch, 0, &denied));
    check(denied == NULL);
    check(!oreslang_source_actor_frame_i64_slot_checked(domain, epoch - 1, 0, &denied));
    check(denied == NULL);
    const int64_t result = oreslang_actor_Worker_on_message(heap, domain, epoch, input);
    atomic_store_explicit(&results[index], result, memory_order_release);
    atomic_fetch_add_explicit(&deliveries[index], 1, memory_order_release);
    return result;
}

static void* spawn(void) {
    void* actor = oreslang_source_actor_spawn(initialize, message, 1);
    check(actor != NULL);
    return actor;
}

int main(void) {
    /* Sequential readiness establishes actor-to-observation indexes only;
     * both actors stay simultaneously live while actual DATA is scheduled. */
    void* failed = spawn();
    wait_for(&ready, 1);
    void* sibling = spawn();
    wait_for(&ready, 2);
    const uintptr_t a = atomic_load_explicit(&addresses[0], memory_order_acquire);
    const uintptr_t b = atomic_load_explicit(&addresses[1], memory_order_acquire);
    check(a && b && (a < b ? b - a >= 65536 : a - b >= 65536));
    check(atomic_load_explicit(&domains[0], memory_order_acquire)
        != atomic_load_explicit(&domains[1], memory_order_acquire));

    oreslang_source_actor_send(sibling, 7);
    wait_for(&deliveries[1], 1);
    check(atomic_load_explicit(&results[1], memory_order_acquire) == 7);
    oreslang_source_actor_send(failed, 100000);
    wait_for(&deliveries[0], 1);
    /* Wait for physical callback exit, fail-stop and reclamation, not just
     * completion of this test's observer callback. */
    oreslang_source_actor_stop(failed);
    oreslang_source_actor_send(sibling, 9);
    wait_for(&deliveries[1], 2);
    check(atomic_load_explicit(&results[1], memory_order_acquire) == 9);

    void* replacement = spawn();
    wait_for(&ready, 3);
    check(atomic_load_explicit(&domains[2], memory_order_acquire)
        != atomic_load_explicit(&domains[0], memory_order_acquire));
    oreslang_source_actor_send(replacement, 11);
    wait_for(&deliveries[2], 1);
    check(atomic_load_explicit(&results[2], memory_order_acquire) == 11);
    oreslang_source_actor_stop(replacement);
    oreslang_source_actor_stop(sibling);
    puts("COMPILED_ACTOR_ISOLATION_PASS actors=3 frame_bytes=65536 sibling=9 replacement=11");
    return 0;
}
