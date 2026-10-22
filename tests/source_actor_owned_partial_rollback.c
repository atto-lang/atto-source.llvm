#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* The script replaces ONLY llvm.trap() at the compiler-generated allocation
 * failure terminal with this hook, keeping all native acquire/drop calls and
 * native heap state unchanged. This is a test-controlled observation boundary,
 * never a guest error or supervisor continuation protocol.
 */
static jmp_buf stop_after_generated_cleanup;
static volatile int failure_trapped = 0;

void ores_test_capture_failure(void) {
    failure_trapped = 1;
    longjmp(stop_after_generated_cleanup, 1);
}

extern void oreslang_actor_Worker_init(
    OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Worker_on_message(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);

int main(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(913, 1, 3);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 913, 1);
    oreslang_actor_Worker_init(heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(heap, turn);

    turn = ores_actor_i64_heap_enter(heap, 913, 1);
    uint64_t resident[3] = {0};
    if (!ores_actor_i64_heap_scoped_try_new(
            heap, turn.domain, turn.epoch, 73, resident)) return 11;
    /* The preexisting actor field and this resident owner occupy two of the
     * three native slots. The handler's first source local can allocate; its
     * second MUST encounter bounded capacity denial, drop the first, trap.
     */
    if (setjmp(stop_after_generated_cleanup) == 0) {
        (void)oreslang_actor_Worker_on_message(
            heap, turn.domain, turn.epoch, 17);
        return 12; /* A source handler must never report normal success. */
    }
    if (failure_trapped != 1) return 13;
    OresActorSlot resident_slot = {resident[0], resident[1], resident[2]};
    if (ores_actor_i64_heap_read(heap, turn, resident_slot) != 73)
        return 14;
    if (ores_actor_i64_heap_field_read(
            heap, turn.domain, turn.epoch, 0) != 0) return 15;

    uint64_t replacement[3] = {0};
    if (!ores_actor_i64_heap_scoped_try_new(
            heap, turn.domain, turn.epoch, 91, replacement))
        return 16; /* First source owner was NOT dropped on allocation failure. */
    OresActorSlot acquired = {replacement[0], replacement[1], replacement[2]};
    if (ores_actor_i64_heap_read(heap, turn, acquired) != 91) return 17;
    uint64_t denied[3] = {0};
    if (ores_actor_i64_heap_scoped_try_new(
            heap, turn.domain, turn.epoch, 92, denied)) return 18;
    if (denied[0] || denied[1] || denied[2]) return 19;

    ores_actor_i64_heap_scoped_drop(
        heap, turn.domain, turn.epoch, replacement);
    ores_actor_i64_heap_scoped_drop(
        heap, turn.domain, turn.epoch, resident);
    ores_actor_i64_heap_leave(heap, turn);
    ores_actor_i64_heap_retire(heap);
    if (ores_actor_i64_heap_try_destroy(heap) != 1) return 20;
    puts("SOURCE_ACTOR_PARTIAL_FAILURE_ROLLBACK_PASS");
    return 0;
}
