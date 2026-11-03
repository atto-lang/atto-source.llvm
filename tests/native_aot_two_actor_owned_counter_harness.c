#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Linked from an actual LLVM IR object emitted for
// tests/fixtures/native-aot-two-actor-owned-counter.ores.
// There are no host replacements for these two generated functions.
extern void oreslang_actor_Worker_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Worker_on_message(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "AOT_TWO_ACTOR_FAIL line %d: %s\n", __LINE__, #test); \
        abort(); \
    } \
} while (0)

static void initialize(OresActorI64Heap* heap, uint64_t actor, uint64_t generation) {
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, actor, generation);
    CHECK(turn.domain != 0 && turn.epoch != 0);
    oreslang_actor_Worker_init(heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(heap, turn);
}

static int64_t dispatch(
        OresActorI64Heap* heap, uint64_t actor, uint64_t generation,
        int64_t increment) {
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, actor, generation);
    const int64_t result = oreslang_actor_Worker_on_message(
        heap, turn.domain, turn.epoch, increment);
    ores_actor_i64_heap_leave(heap, turn);
    return result;
}


#define TRACE_CAPACITY 8192u

// A successful snapshot is only a proof of accesses via the guarded i64
// heap ABI. It is not an allocation-closure or whole-process zero-GC proof.
static uint64_t verify_trace(
        OresActorI64Heap* heap, uint64_t actor_id, uint64_t incarnation,
        uint32_t turns, int external_allocation) {
    const uint32_t expected = 1u + turns * 4u
        + (external_allocation ? 1u : 0u);
    CHECK(expected < TRACE_CAPACITY);
    OresActorHeapAccessEvent* events = (OresActorHeapAccessEvent*)
        calloc(TRACE_CAPACITY, sizeof(*events));
    CHECK(events != NULL);
    OresActorHeapAccessSummary summary = {0};
    CHECK(ores_actor_i64_heap_trace_snapshot(
        heap, events, TRACE_CAPACITY, &summary) == 1);
    CHECK(summary.enabled == 1 && summary.dropped == 0);
    CHECK(summary.capacity == TRACE_CAPACITY);
    CHECK(summary.stored == expected);
    CHECK(events[0].kind == ORES_ACTOR_HEAP_ACCESS_FIELD_INIT);
    CHECK(events[0].turn_epoch == 1 && events[0].slot_index == 0);
    const uint64_t domain = events[0].domain;
    CHECK(domain != 0);
    for (uint32_t j = 0; j < summary.stored; ++j) {
        CHECK(events[j].domain == domain);
        CHECK(events[j].actor_id == actor_id);
        CHECK(events[j].incarnation == incarnation);
        CHECK(events[j].slot_generation != 0);
    }
    // One scratch slot may be recycled on every turn. A reused native
    // handle must always carry a strictly newer generation, so stale handles
    // can never alias the new owner of the same slot index.
    uint64_t last_temporary_generation[8] = {0};
    for (uint32_t j = 0; j < turns; ++j) {
        const OresActorHeapAccessEvent* batch = &events[1u + j * 4u];
        const uint64_t epoch = (uint64_t)j + 2;
        for (unsigned k = 0; k < 4; ++k)
            CHECK(batch[k].turn_epoch == epoch);
        CHECK(batch[0].kind == ORES_ACTOR_HEAP_ACCESS_FIELD_READ);
        CHECK(batch[1].kind == ORES_ACTOR_HEAP_ACCESS_ALLOC);
        CHECK(batch[2].kind == ORES_ACTOR_HEAP_ACCESS_FIELD_WRITE);
        CHECK(batch[3].kind == ORES_ACTOR_HEAP_ACCESS_DROP);
        CHECK(batch[0].slot_index == 0 && batch[2].slot_index == 0);
        CHECK(batch[1].slot_index != 0);
        CHECK(batch[3].slot_index == batch[1].slot_index);
        CHECK(batch[3].slot_generation == batch[1].slot_generation);
        CHECK(batch[0].slot_generation == events[0].slot_generation);
        CHECK(batch[2].slot_generation == events[0].slot_generation);
        CHECK(batch[1].slot_index < 8u);
        CHECK(batch[1].slot_generation >
              last_temporary_generation[batch[1].slot_index]);
        last_temporary_generation[batch[1].slot_index] =
            batch[1].slot_generation;
    }
    if (external_allocation) {
        const OresActorHeapAccessEvent* extra = &events[expected - 1u];
        CHECK(extra->kind == ORES_ACTOR_HEAP_ACCESS_ALLOC);
        CHECK(extra->turn_epoch == (uint64_t)turns + 2);
        CHECK(extra->slot_index != 0);
        CHECK(extra->slot_index < 8u);
        CHECK(extra->slot_generation >
              last_temporary_generation[extra->slot_index]);
    }
    free(events);
    return domain;
}

int main(void) {
    const uint64_t a_id = 1001, b_id = 2002;
    const uint64_t a_generation = 7, b_generation = 11;
    // One compiler-defined persistent field + one bounded scoped owned
    // temporary; A additionally retains a distinct externally owned loan.
    OresActorI64Heap* a = ores_actor_i64_heap_open(a_id, a_generation, 8);
    OresActorI64Heap* b = ores_actor_i64_heap_open(b_id, b_generation, 8);
    CHECK(a != NULL && b != NULL && a != b);
    CHECK(ores_actor_i64_heap_trace_enable(a, TRACE_CAPACITY) == 1);
    CHECK(ores_actor_i64_heap_trace_enable(b, TRACE_CAPACITY) == 1);
    initialize(a, a_id, a_generation);
    initialize(b, b_id, b_generation);

    for (int64_t i = 1; i <= 1000; ++i) {
        CHECK(dispatch(a, a_id, a_generation, 1) == i);
        CHECK(dispatch(b, b_id, b_generation, 3) == 3 * i);
    }

    // Simulate an external lifetime-only lease, not a guest-visible pointer.
    OresActorTurn turn = ores_actor_i64_heap_enter(a, a_id, a_generation);
    OresActorSlot external_slot = ores_actor_i64_heap_new(a, turn, 1234);
    OresActorLoan external_loan = ores_actor_i64_heap_borrow(a, turn, external_slot);
    CHECK(external_loan.nonce != 0 && external_loan.domain == turn.domain);
    ores_actor_i64_heap_leave(a, turn);

    const uint64_t trace_domain_a = verify_trace(
        a, a_id, a_generation, 1000u, 1);
    ores_actor_i64_heap_retire(a);
    CHECK(ores_actor_i64_heap_try_destroy(a) == 0);

    // B remains mutable after A has STOPPED; B must retain its own total.
    for (int64_t i = 1; i <= 8; ++i)
        CHECK(dispatch(b, b_id, b_generation, 2) == 3000 + 2 * i);

    // Only the retired A loan is released. A cannot be accessed after
    // destruction, and B remains independently alive through final teardown.
    ores_actor_i64_heap_release(a, external_loan);
    CHECK(ores_actor_i64_heap_try_destroy(a) == 1);
    CHECK(dispatch(b, b_id, b_generation, 0) == 3016);

    const uint64_t trace_domain_b = verify_trace(
        b, b_id, b_generation, 1009u, 0);
    CHECK(trace_domain_a != trace_domain_b);
    ores_actor_i64_heap_retire(b);
    CHECK(ores_actor_i64_heap_try_destroy(b) == 1);
    puts("AOT_COMPILED_TWO_ACTOR_OWNED_HEAPS_2009_TURNS_PASS");
    return 0;
}
