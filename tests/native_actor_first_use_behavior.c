/*
 * Compiled guest callback behavioral regression, NOT transport admission proof.
 * The test-only tagged-i64 decoder supplies a post-admission payload directly
 * to the actual generated actor method. NativeActorScheduler / MessagePack
 * authentication remain covered by separate source-actor pipeline tests.
 *
 * Runs real emitted LLVM plus the real ActorOwnedI64Heap.c, checking two
 * admitted physical turns, persistent state, ordered writes and result copies.
 */
#include "oreslang/runtime/ActorOwnedI64Heap.h"

#include <inttypes.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void oreslang_actor_Probe_init(OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Probe_receive(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t, const void*, uint64_t);

/* Only this isolated direct-ABI test may bypass the trusted MessagePack
 * decoder. Any production scheduler or wire tests must link the real one. */
int64_t oreslang_source_actor_tagged_i64(const uint8_t* bytes, size_t size) {
    if (!bytes || size != sizeof(int64_t)) abort();
    int64_t value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static int expect_turn(OresActorI64Heap* heap, OresActorTurn turn,
                       int64_t message, int64_t expected_first,
                       int64_t expected_second) {
    const int64_t actual = oreslang_actor_Probe_receive(
        heap, turn.domain, turn.epoch, 0, &message, sizeof(message));
    const int64_t first = ores_actor_i64_heap_field_read(
        heap, turn.domain, turn.epoch, 0);
    const int64_t second = ores_actor_i64_heap_field_read(
        heap, turn.domain, turn.epoch, 1);
    if (actual != expected_second || first != expected_first
            || second != expected_second) {
        fprintf(stderr,
            "FIRST_USE_BEHAVIOR_FAIL result=%" PRId64
            " first=%" PRId64 " second=%" PRId64
            " expected_first=%" PRId64 " expected_second=%" PRId64 "\n",
            actual, first, second, expected_first, expected_second);
        return 0;
    }
    return 1;
}

int main(void) {
    const uint64_t actor_id = 101;
    const uint64_t incarnation = 1;
    OresActorI64Heap* heap = ores_actor_i64_heap_open(actor_id, incarnation, 2);

    OresActorTurn first = ores_actor_i64_heap_enter(heap, actor_id, incarnation);
    oreslang_actor_Probe_init(heap, first.domain, first.epoch);
    if (!expect_turn(heap, first, 4, 5, 12)) return 11;
    ores_actor_i64_heap_leave(heap, first);

    OresActorTurn second = ores_actor_i64_heap_enter(heap, actor_id, incarnation);
    if (!expect_turn(heap, second, 2, 3, 15)) return 12;
    ores_actor_i64_heap_leave(heap, second);

    ores_actor_i64_heap_retire(heap);
    if (!ores_actor_i64_heap_try_destroy(heap)) return 13;
    fprintf(stderr, "FIRST_USE_BEHAVIOR_PASS turns=2 result=15\n");
    return 0;
}
