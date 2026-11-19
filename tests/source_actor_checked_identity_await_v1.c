/*
 * Production native VM test: a checked ID is passed through root CONTROL
 * ready/done continuations as an integer identity, never a host pointer.
 */
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static atomic_uint_fast64_t current_id;
static atomic_uint_fast64_t previous_id;
static atomic_int ctor_count;
static atomic_int ready_calls;
static atomic_int done_calls;

static void check(int ok) {
    if (!ok) {
        fputs("SOURCE_ACTOR_CHECKED_ID_AWAIT_INVARIANT_FAILED\n", stderr);
        abort();
    }
}
static void init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    check(heap && domain && epoch);
    atomic_fetch_add_explicit(&ctor_count, 1, memory_order_release);
}
static int64_t receive(OresActorI64Heap* heap, uint64_t domain,
                       uint64_t epoch, int64_t payload) {
    check(heap && domain && epoch);
    return payload;
}
static int64_t ready_continuation(OresSourceActorId id) {
    const OresSourceActorId current =
        atomic_load_explicit(&current_id, memory_order_acquire);
    const OresSourceActorId previous =
        atomic_load_explicit(&previous_id, memory_order_acquire);
    check(id == current && id != 0);
    check(oreslang_source_actor_ready_status_id(id)
          == ORES_SOURCE_ACTOR_STARTUP_READY);
    if (previous) {
        check(oreslang_source_actor_ready_status_id(previous) == -1);
        check(!oreslang_source_actor_try_send_id(previous, 9));
    }
    check(oreslang_source_actor_try_send_id(id, 5));
    atomic_fetch_add_explicit(&ready_calls, 1, memory_order_acq_rel);
    return 43;
}
static int64_t done_continuation(OresSourceActorId id) {
    const OresSourceActorId current =
        atomic_load_explicit(&current_id, memory_order_acquire);
    check(id == current && id != 0);
    check(!oreslang_source_actor_try_send_id(id, 7));
    check(!oreslang_source_actor_try_request_stop_id(id));
    atomic_fetch_add_explicit(&done_calls, 1, memory_order_acq_rel);
    return 44;
}
int main(void) {
    oreslang_source_actor_test_force_address_reuse(1);
    OresSourceActorId previous = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const OresSourceActorId current = oreslang_source_actor_spawn_id(
            init, receive, 1);
        check(current && current > previous);
        check(oreslang_source_actor_test_address_reuses() == i);
        atomic_store_explicit(&current_id, current, memory_order_release);
        atomic_store_explicit(&previous_id, previous, memory_order_release);
        const int64_t ready = oreslang_source_actor_await_ready_id(
            current, ready_continuation, UINT64_C(0x1000) + i);
        check(ready == 43);
        oreslang_source_actor_request_stop_id(current);
        const int64_t done = oreslang_source_actor_await_done_id(
            current, done_continuation, UINT64_C(0x2000) + i);
        check(done == 44);
        check(oreslang_source_actor_ready_status_id(current) == -1);
        check(!oreslang_source_actor_try_send_id(current, 5));
        check(!oreslang_source_actor_try_request_stop_id(current));
        previous = current;
    }
    check(atomic_load_explicit(&ctor_count, memory_order_acquire) == 16);
    check(atomic_load_explicit(&ready_calls, memory_order_acquire) == 16);
    check(atomic_load_explicit(&done_calls, memory_order_acquire) == 16);
    check(oreslang_source_actor_test_address_reuses() == 15);
    oreslang_source_actor_test_force_address_reuse(0);
    puts("SOURCE_ACTOR_CHECKED_ID_AWAIT_PASS|actors=16|reused_addresses=15|ready_callbacks=16|done_callbacks=16|physical_joins=16");
    return 0;
}
