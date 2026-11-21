#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>

/* Runtime-only contract: checked allocation never partially publishes state
 * on quota failure; verified source lowering is tested by the separate
 * real-compiler source_actor_owned_drop_stress.py matrix. */
static int verify(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(712, 1, 2);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 712, 1);
    uint64_t a[3] = {0}, b[3] = {0}, denied[3] = {0};
    if (!ores_actor_i64_heap_scoped_try_new(
            heap, turn.domain, turn.epoch, 41, a)) return 11;
    if (!ores_actor_i64_heap_scoped_try_new(
            heap, turn.domain, turn.epoch, 43, b)) return 12;
    if (ores_actor_i64_heap_scoped_try_new(
            heap, turn.domain, turn.epoch, 99, denied)) return 13;
    if (denied[0] || denied[1] || denied[2]) return 14;
    OresActorSlot first = {a[0], a[1], a[2]};
    OresActorSlot second = {b[0], b[1], b[2]};
    if (ores_actor_i64_heap_read(heap, turn, first) != 41) return 15;
    if (ores_actor_i64_heap_read(heap, turn, second) != 43) return 16;

    /* Reclamation of the most recently created owner must make exactly
     * one slot available, preserving the older persistent allocation. */
    ores_actor_i64_heap_scoped_drop(heap, turn.domain, turn.epoch, b);
    if (b[0] || b[1] || b[2]) return 17;
    if (!ores_actor_i64_heap_scoped_try_new(
            heap, turn.domain, turn.epoch, 47, denied)) return 18;
    OresActorSlot reused = {denied[0], denied[1], denied[2]};
    if (reused.index != second.index ||
            reused.generation <= second.generation ||
            ores_actor_i64_heap_read(heap, turn, reused) != 47) return 19;
    if (ores_actor_i64_heap_read(heap, turn, first) != 41) return 20;
    ores_actor_i64_heap_scoped_drop(heap, turn.domain, turn.epoch, denied);
    ores_actor_i64_heap_scoped_drop(heap, turn.domain, turn.epoch, a);
    ores_actor_i64_heap_leave(heap, turn);

    /* Repeated admitted turns cannot reuse a stale slot generation. */
    for (int i = 0; i < 10000; ++i) {
        turn = ores_actor_i64_heap_enter(heap, 712, 1);
        if (!ores_actor_i64_heap_scoped_try_new(
                heap, turn.domain, turn.epoch, i, a)) return 21;
        OresActorSlot live = {a[0], a[1], a[2]};
        if (ores_actor_i64_heap_read(heap, turn, live) != i) return 22;
        ores_actor_i64_heap_scoped_drop(heap, turn.domain, turn.epoch, a);
        ores_actor_i64_heap_leave(heap, turn);
    }
    ores_actor_i64_heap_retire(heap);
    if (ores_actor_i64_heap_try_destroy(heap) != 1) return 23;
    return 0;
}

int main(void) {
    const int result = verify();
    if (result) {
        fprintf(stderr, "SOURCE_ACTOR_SCOPED_ALLOC_STATUS_FAIL=%d\n", result);
        return result;
    }
    puts("SOURCE_ACTOR_SCOPED_ALLOC_STATUS_10000_PASS");
    return 0;
}
