#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdint.h>
#include <stdio.h>

extern void oreslang_actor_Stage0_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Stage0_receive(OresActorI64Heap*, uint64_t, uint64_t,
                                           uint64_t, const uint8_t*, uint64_t);
#define VERIFY(value) do { if (!(value)) { \
    fprintf(stderr, "unbound ingress assertion failed at %d\n", __LINE__); return 2; \
} } while (0)

int main(void) {
    VERIFY(oreslang_source_actor_pipeline_ingress() == UINT64_MAX);
    OresActorI64Heap* heap = ores_actor_i64_heap_open(17, 5, 1);
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 17, 5);
    oreslang_actor_Stage0_init(heap, turn.domain, turn.epoch);
    /* An intact heap turn is not DATA authority. A NULL frame would abort if
     * user payload decoding or state callback entry were allowed here. */
    VERIFY(oreslang_actor_Stage0_receive(heap, turn.domain, turn.epoch, 0, NULL, 0) == -1);
    VERIFY(ores_actor_i64_heap_field_read(heap, turn.domain, turn.epoch, 0) == 0);
    ores_actor_i64_heap_leave(heap, turn);
    ores_actor_i64_heap_retire(heap);
    VERIFY(ores_actor_i64_heap_try_destroy(heap) == 1);
    puts("PIPE_UNBOUND_INGRESS_REJECTED_BEFORE_HANDLER");
    return 43;
}
