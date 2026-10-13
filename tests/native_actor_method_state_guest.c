#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>

extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_get_handle(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_get_using(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_increment_handle(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_increment_using(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_reset(OresActorI64Heap*, uint64_t, uint64_t);

int main(void) {
    const uint64_t actor_id = 997;
    const uint64_t incarnation = 1;
    OresActorI64Heap* heap = ores_actor_i64_heap_open(actor_id, incarnation, 1);
    const OresActorTurn turn =
        ores_actor_i64_heap_enter(heap, actor_id, incarnation);
    oreslang_actor_Counter_init(heap, turn.domain, turn.epoch);
    if (oreslang_actor_Counter_get_handle(heap, turn.domain, turn.epoch) != 7
        || oreslang_actor_Counter_get_using(heap, turn.domain, turn.epoch) != 7
        || oreslang_actor_Counter_increment_handle(heap, turn.domain, turn.epoch) != 8
        || oreslang_actor_Counter_increment_using(heap, turn.domain, turn.epoch) != 9
        || oreslang_actor_Counter_reset(heap, turn.domain, turn.epoch) != 12
        || oreslang_actor_Counter_get_using(heap, turn.domain, turn.epoch) != 12) {
        fprintf(stderr, "ACTOR_METHOD_STATE_GUEST_MISMATCH\n");
        return 1;
    }
    ores_actor_i64_heap_leave(heap, turn);
    ores_actor_i64_heap_retire(heap);
    if (!ores_actor_i64_heap_try_destroy(heap)) {
        fprintf(stderr, "ACTOR_METHOD_STATE_GUEST_TEARDOWN_FAIL\n");
        return 2;
    }
    fprintf(stderr, "ACTOR_METHOD_STATE_GUEST_PASS read=7 increment=8,9 reset=12\n");
    return 43;
}
