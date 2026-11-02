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

int main(void) {
    const uint64_t a_id = 1001, b_id = 2002;
    const uint64_t a_generation = 7, b_generation = 11;
    // One compiler-defined persistent field + one bounded scoped owned
    // temporary; A additionally retains a distinct externally owned loan.
    OresActorI64Heap* a = ores_actor_i64_heap_open(a_id, a_generation, 8);
    OresActorI64Heap* b = ores_actor_i64_heap_open(b_id, b_generation, 8);
    CHECK(a != NULL && b != NULL && a != b);
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

    ores_actor_i64_heap_retire(b);
    CHECK(ores_actor_i64_heap_try_destroy(b) == 1);
    puts("AOT_COMPILED_TWO_ACTOR_OWNED_HEAPS_2009_TURNS_PASS");
    return 0;
}
