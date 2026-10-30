/* Execute actual compiler-emitted isolated actor code against actor-owned heap.
 * No reimplementation of the guest handler: symbols are defined in LLVM IR.
 * Host controls (actor, incarnation, epoch), never source-supplied values.
 * This synchronous proof does NOT exercise suspended actor-state lowering. */
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern void oreslang_actor_Counter_init(
    OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_on_message(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);

static void require(int ok) { if (!ok) abort(); }

int main(int argc, char** argv) {
    require(argc == 2);
    const int writing = argv[1][0] == 'w' && argv[1][1] == '\0';
    const int reading = argv[1][0] == 'r' && argv[1][1] == '\0';
    require(writing || reading);

    OresActorI64Heap* heap = ores_actor_i64_heap_open(912, 1, 1);
    require(heap != NULL);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 912, 1);
    oreslang_actor_Counter_init(heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(heap, turn);

    turn = ores_actor_i64_heap_enter(heap, 912, 1);
    const int64_t first = oreslang_actor_Counter_on_message(
        heap, turn.domain, turn.epoch, 1);
    ores_actor_i64_heap_leave(heap, turn);
    turn = ores_actor_i64_heap_enter(heap, 912, 1);
    const int64_t second = oreslang_actor_Counter_on_message(
        heap, turn.domain, turn.epoch, 2);
    ores_actor_i64_heap_leave(heap, turn);

    require(first == (writing ? 1 : 8));
    require(second == (writing ? 3 : 9));
    ores_actor_i64_heap_retire(heap);
    require(ores_actor_i64_heap_try_destroy(heap));
    puts(writing ? "COMPILED_SEALED_WRITE_GUEST_PASS"
                 : "COMPILED_SEALED_READ_GUEST_PASS");
    return 0;
}
