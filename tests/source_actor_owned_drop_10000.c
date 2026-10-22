#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Link these symbols from LLVM IR produced by the actual Oreslang compiler.
 * This driver cannot fake native actor source lowering or use host drop glue.
 */
extern void oreslang_actor_Worker_init(
    OresActorI64Heap*, uint64_t domain, uint64_t epoch);
extern int64_t oreslang_actor_Worker_on_message(
    OresActorI64Heap*, uint64_t domain, uint64_t epoch, int64_t message);

int main(void) {
    const uint64_t owner = UINT64_C(4101);
    const uint64_t incarnation = UINT64_C(1);
    /* One immutable/native field plus exactly two recyclable owner slots.
     * An omitted compiler drop exhausts capacity at the next message.
     */
    OresActorI64Heap* heap = ores_actor_i64_heap_open(owner, incarnation, 3);
    OresActorTurn admission = ores_actor_i64_heap_enter(heap, owner, incarnation);
    oreslang_actor_Worker_init(heap, admission.domain, admission.epoch);
    ores_actor_i64_heap_leave(heap, admission);

    uint64_t expected = 0;
    for (uint64_t i = 0; i < UINT64_C(10000); ++i) {
        admission = ores_actor_i64_heap_enter(heap, owner, incarnation);
        const uint64_t input = i % UINT64_C(7) + UINT64_C(1);
        const uint64_t received = (uint64_t)
            oreslang_actor_Worker_on_message(
                heap, admission.domain, admission.epoch, (int64_t)input);
        expected = (expected + input) * UINT64_C(2);
        if (received != expected) {
            fprintf(stderr,
                "SOURCE_ACTOR_DROP_BAD_RESULT at turn %llu\n",
                (unsigned long long)i);
            return 71;
        }
        ores_actor_i64_heap_leave(heap, admission);
    }
    /* The actor field remains intact after every temporary drop.
     * The final native actor state is only accessed under an admitted turn.
     */
    admission = ores_actor_i64_heap_enter(heap, owner, incarnation);
    const int64_t last = ores_actor_i64_heap_field_read(
        heap, admission.domain, admission.epoch, 0);
    if ((uint64_t)last != expected) return 72;
    ores_actor_i64_heap_leave(heap, admission);
    ores_actor_i64_heap_retire(heap);
    if (ores_actor_i64_heap_try_destroy(heap) != 1) return 73;
    puts("SOURCE_ACTOR_COMPILER_DROP_10000_TURNS_PASS");
    return 0;
}
