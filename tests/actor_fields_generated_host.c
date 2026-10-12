#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
#include <threads.h>

/* Generated LLVM symbols are host-only. They are not Oreslang builtins. */
extern void* ores_actor_Counter_init(
    OresActorI64Heap* heap, uint64_t domain, uint64_t epoch);
extern int64_t ores_actor_Counter_add(
    OresActorI64Heap* heap, uint64_t domain, uint64_t epoch,
    void* fields, int64_t by);
extern void ores_actor_Counter_drop(
    OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, void* fields);

typedef struct {
    OresActorI64Heap* heap;
    void* fields;
    int failed;
} Carrier;

static int migrated_turn(void* arg) {
    Carrier* carrier = (Carrier*)arg;
    OresActorTurn turn = ores_actor_i64_heap_enter(carrier->heap, 51, 3);
    if (ores_actor_Counter_add(carrier->heap, turn.domain, turn.epoch,
                                carrier->fields, 10) != 42) {
        carrier->failed = 1;
    }
    ores_actor_i64_heap_leave(carrier->heap, turn);
    return 0;
}

int main(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(51, 3, 16);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 51, 3);
    void* fields = ores_actor_Counter_init(heap, turn.domain, turn.epoch);
    if (!fields || ores_actor_Counter_add(
            heap, turn.domain, turn.epoch, fields, 20) != 32) return 11;
    ores_actor_i64_heap_leave(heap, turn);

    Carrier carrier = {heap, fields, 0};
    thrd_t other;
    if (thrd_create(&other, migrated_turn, &carrier) != thrd_success) return 12;
    int status = -1;
    if (thrd_join(other, &status) != thrd_success) return 13;
    if (status != 0 || carrier.failed) return 14;

    turn = ores_actor_i64_heap_enter(heap, 51, 3);
    ores_actor_Counter_drop(heap, turn.domain, turn.epoch, fields);
    ores_actor_i64_heap_leave(heap, turn);
    ores_actor_i64_heap_retire(heap);
    if (ores_actor_i64_heap_try_destroy(heap) != 1) return 15;
    return 0;
}
