// Clang-linked with the *actual emitted LLVM IR* for the Counter actor.
// The host supplies private, valid turn tickets and never synthesizes the
// actor method, field mutation, or result. This proves four real compiled
// guest handler invocations, though not declare/state guest syntax yet.
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>

extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_on_message(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);

static int call(OresActorI64Heap* heap, uint64_t id, uint64_t incarnation,
                int64_t message, int64_t expected) {
    const OresActorTurn turn = ores_actor_i64_heap_enter(heap, id, incarnation);
    if (turn.domain == 0 || turn.epoch == 0) return 1;
    const int64_t actual = oreslang_actor_Counter_on_message(
        heap, turn.domain, turn.epoch, message);
    ores_actor_i64_heap_leave(heap, turn);
    return actual != expected;
}

static int initialize(OresActorI64Heap* heap, uint64_t id, uint64_t incarnation) {
    const OresActorTurn turn = ores_actor_i64_heap_enter(heap, id, incarnation);
    if (turn.domain == 0 || turn.epoch == 0) return 1;
    oreslang_actor_Counter_init(heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(heap, turn);
    return 0;
}

int main(void) {
    const uint64_t left_id = 1001, right_id = 1002, incarnation = 7;
    OresActorI64Heap* left = ores_actor_i64_heap_open(left_id, incarnation, 3);
    OresActorI64Heap* right = ores_actor_i64_heap_open(right_id, incarnation, 3);
    if (!left || !right) return 10;
    if (initialize(left, left_id, incarnation) != 0
            || initialize(right, right_id, incarnation) != 0) return 11;
    if (call(left, left_id, incarnation, 1, 1)
            || call(right, right_id, incarnation, 7, 7)
            || call(left, left_id, incarnation, 2, 3)
            || call(right, right_id, incarnation, 8, 15)
            || call(left, left_id, incarnation, 3, 6)
            || call(left, left_id, incarnation, 4, 10)) return 12;
    // Re-read both independent actor heaps with zero increments.
    if (call(left, left_id, incarnation, 0, 10)
            || call(right, right_id, incarnation, 0, 15)) return 13;
    ores_actor_i64_heap_retire(left);
    ores_actor_i64_heap_retire(right);
    if (!ores_actor_i64_heap_try_destroy(left)
            || !ores_actor_i64_heap_try_destroy(right)) return 14;
    fputs("LLVM_COUNTER_FOUR_VERIFIED left=10 right=15 turns=8\n", stderr);
    return 0;
}
