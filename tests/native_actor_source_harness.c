#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdlib.h>
extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_on_message(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
int main(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(123, 7, 3);
    OresActorTurn init = ores_actor_i64_heap_enter(heap, 123, 7);
    oreslang_actor_Counter_init(heap, init.domain, init.epoch);
    ores_actor_i64_heap_leave(heap, init);
    // Fields persist and later mailbox turns may run on another native carrier.
    OresActorTurn next = ores_actor_i64_heap_enter(heap, 123, 7);
    if (oreslang_actor_Counter_on_message(heap, next.domain, next.epoch, 3) != 45) return 14;
    ores_actor_i64_heap_leave(heap, next);
    OresActorTurn last = ores_actor_i64_heap_enter(heap, 123, 7);
    if (oreslang_actor_Counter_on_message(heap, last.domain, last.epoch, 2) != 89) return 16;
    ores_actor_i64_heap_leave(heap, last);
    ores_actor_i64_heap_retire(heap);
    if (ores_actor_i64_heap_try_destroy(heap) != 1) return 13;
    return 0;
}
