/* Trusted ABI harness: separate, non-overlapping actor turns on new carriers. */
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

extern void oreslang_actor_Counter_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Counter_set(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
extern int64_t oreslang_actor_Counter_increment(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
extern int64_t oreslang_actor_Counter_read(OresActorI64Heap*, uint64_t, uint64_t);

struct Invocation { OresActorI64Heap* heap; int action; int64_t expected; };

static void* invoke(void* raw) {
    struct Invocation* call = raw;
    OresActorTurn turn = ores_actor_i64_heap_enter(call->heap, 123, 7);
    int64_t result;
    switch (call->action) {
        case 0:
            oreslang_actor_Counter_init(call->heap, turn.domain, turn.epoch);
            result = 0;
            break;
        case 1:
            result = oreslang_actor_Counter_set(call->heap, turn.domain, turn.epoch, 10);
            break;
        case 2:
            result = oreslang_actor_Counter_increment(call->heap, turn.domain, turn.epoch, 2);
            break;
        default:
            result = oreslang_actor_Counter_read(call->heap, turn.domain, turn.epoch);
    }
    ores_actor_i64_heap_leave(call->heap, turn);
    if (result != call->expected) abort();
    return NULL;
}

int main(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(123, 7, 2);
    const int64_t expected[] = {0, 10, 24, 36};
    for (int action = 0; action < 4; ++action) {
        struct Invocation call = {heap, action, expected[action]};
        pthread_t thread;
        if (pthread_create(&thread, NULL, invoke, &call) != 0) return 90;
        if (pthread_join(thread, NULL) != 0) return 91;
    }
    ores_actor_i64_heap_retire(heap);
    return ores_actor_i64_heap_try_destroy(heap) == 1 ? 0 : 92;
}
