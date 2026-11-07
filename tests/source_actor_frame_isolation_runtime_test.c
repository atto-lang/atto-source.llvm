#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <time.h>

static atomic_int constructors;
static atomic_int deliveries;
static _Atomic(uintptr_t) addresses[3];
static atomic_uint_fast64_t domains[3];

static void check(int condition) { if (!condition) abort(); }

static int wait_for(atomic_int* counter, int target) {
    struct timespec start = {0, 0}, now = {0, 0};
    check(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    do {
        if (atomic_load_explicit(counter, memory_order_acquire) >= target)
            return 1;
        sched_yield();
        check(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    } while (now.tv_sec - start.tv_sec < 5);
    return 0;
}

static void initialize(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain && epoch);
    const int position = atomic_fetch_add_explicit(
        &constructors, 1, memory_order_acq_rel);
    check(position >= 0 && position < 3);
    int64_t* current = NULL;
    check(oreslang_source_actor_frame_i64_slot_checked(
        domain, epoch, 0, &current) && current != NULL);
    check(*current == 0);
    *current = 400 + position;

    if (position == 2) {
        // A recycled ActorRef/actor ID may not grant authority to a new
        // incarnation even if the native allocator reuses the same address.
        int64_t* foreign = current;
        const uint64_t older = atomic_load_explicit(
            &domains[0], memory_order_acquire);
        check(older != 0 && older != domain);
        check(!oreslang_source_actor_frame_i64_slot_checked(
            older, epoch, 0, &foreign) && foreign == NULL);
    }
    atomic_store_explicit(&addresses[position],
        (uintptr_t)current, memory_order_release);
    atomic_store_explicit(&domains[position], domain, memory_order_release);
}

static int64_t message(
        OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
        int64_t payload) {
    (void)heap;
    int64_t* slot = NULL;
    check(oreslang_source_actor_frame_i64_slot_checked(
        domain, epoch, 0, &slot) && slot != NULL);
    check(*slot == 0); // constructor and prior message frames were scrubbed
    *slot = payload;
    check(*oreslang_source_actor_frame_i64_slot(domain, epoch, 0) == payload);
    atomic_fetch_add_explicit(&deliveries, 1, memory_order_release);
    return *slot;
}

int main(void) {
    void* actor_a = oreslang_source_actor_spawn(initialize, message, 1);
    void* actor_b = oreslang_source_actor_spawn(initialize, message, 1);
    check(actor_a && actor_b);
    check(wait_for(&constructors, 2));

    const uintptr_t first = atomic_load_explicit(
        &addresses[0], memory_order_acquire);
    const uintptr_t second = atomic_load_explicit(
        &addresses[1], memory_order_acquire);
    check(first != 0 && second != 0 && first != second);
    check(atomic_load_explicit(&domains[0], memory_order_acquire)
        != atomic_load_explicit(&domains[1], memory_order_acquire));

    oreslang_source_actor_send(actor_a, 7);
    oreslang_source_actor_send(actor_b, 11);
    check(wait_for(&deliveries, 2));

    oreslang_source_actor_stop(actor_a);
    int64_t* unauthorized = NULL;
    check(!oreslang_source_actor_frame_i64_slot_checked(
        atomic_load_explicit(&domains[0], memory_order_acquire),
        1, 0, &unauthorized) && unauthorized == NULL);
    oreslang_source_actor_stop(actor_b);

    // The next actor has a fresh heap/domain and frame generation even if
    // malloc recycles the address of a retired frame allocation.
    void* actor_c = oreslang_source_actor_spawn(initialize, message, 1);
    check(actor_c && wait_for(&constructors, 3));
    const uint64_t third = atomic_load_explicit(
        &domains[2], memory_order_acquire);
    check(third != 0 &&
        third != atomic_load_explicit(&domains[0], memory_order_acquire) &&
        third != atomic_load_explicit(&domains[1], memory_order_acquire));
    oreslang_source_actor_send(actor_c, 17);
    check(wait_for(&deliveries, 3));
    oreslang_source_actor_stop(actor_c);

    puts("SOURCE_ACTOR_FRAME_ISOLATION_RUNTIME_PASS actors=3 generations=3");
    return 0;
}
