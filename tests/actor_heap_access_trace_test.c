#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "actor heap trace contract failed at line %d: %s\n", __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static void test_bounded_successful_accesses(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(1201, 7, 8);
    OresActorHeapAccessSummary summary = {0};
    OresActorHeapAccessEvent records[7];
    memset(records, 0, sizeof(records));
    REQUIRE(!ores_actor_i64_heap_trace_snapshot(heap, records, 7, &summary));
    REQUIRE(!ores_actor_i64_heap_trace_enable(heap, 0));
    REQUIRE(!ores_actor_i64_heap_trace_enable(heap, 65537));
    REQUIRE(ores_actor_i64_heap_trace_enable(heap, 7));
    REQUIRE(!ores_actor_i64_heap_trace_enable(heap, 7));

    OresActorTurn first = ores_actor_i64_heap_enter(heap, 1201, 7);
    REQUIRE(!ores_actor_i64_heap_trace_snapshot(heap, records, 7, &summary));
    REQUIRE(!ores_actor_i64_heap_trace_enable(heap, 2));
    OresActorSlot slot = ores_actor_i64_heap_new(heap, first, 19);
    REQUIRE(ores_actor_i64_heap_read(heap, first, slot) == 19);
    ores_actor_i64_heap_write(heap, first, slot, 20);
    ores_actor_i64_heap_drop(heap, first, slot);
    ores_actor_i64_heap_field_init_mutable(heap, first.domain, first.epoch, 3, 55);
    REQUIRE(ores_actor_i64_heap_field_read(heap, first.domain, first.epoch, 3) == 55);
    ores_actor_i64_heap_field_write(heap, first.domain, first.epoch, 3, 57);
    ores_actor_i64_heap_leave(heap, first);
    REQUIRE(ores_actor_i64_heap_trace_snapshot(heap, records, 7, &summary));
    REQUIRE(summary.enabled && summary.capacity == 7 && summary.stored == 7 && summary.dropped == 0);
    const uint32_t kinds[7] = {
        ORES_ACTOR_HEAP_ACCESS_ALLOC, ORES_ACTOR_HEAP_ACCESS_READ,
        ORES_ACTOR_HEAP_ACCESS_WRITE, ORES_ACTOR_HEAP_ACCESS_DROP,
        ORES_ACTOR_HEAP_ACCESS_FIELD_INIT, ORES_ACTOR_HEAP_ACCESS_FIELD_READ,
        ORES_ACTOR_HEAP_ACCESS_FIELD_WRITE
    };
    for (uint32_t i = 0; i < 7; ++i) {
        REQUIRE(records[i].domain == first.domain);
        REQUIRE(records[i].actor_id == 1201 && records[i].incarnation == 7);
        REQUIRE(records[i].turn_epoch == first.epoch);
        REQUIRE(records[i].kind == kinds[i]);
        REQUIRE(records[i].slot_generation == 1);
        REQUIRE(records[i].slot_index == (i < 4 ? slot.index : 3));
    }
    REQUIRE(!ores_actor_i64_heap_trace_snapshot(heap, records, 6, &summary));
    /* Another turn has a new epoch, and retired slot generations are never
     * laundered into the first incarnation's earlier access identities. */
    OresActorTurn second = ores_actor_i64_heap_enter(heap, 1201, 7);
    REQUIRE(second.epoch != first.epoch);
    REQUIRE(ores_actor_i64_heap_field_read(heap, second.domain, second.epoch, 3) == 57);
    ores_actor_i64_heap_leave(heap, second);
    /* Overflow invalidates certification even though the host still receives
     * diagnostic events and the sticky dropped counter. */
    REQUIRE(!ores_actor_i64_heap_trace_snapshot(heap, records, 7, &summary));
    REQUIRE(summary.stored == 7 && summary.dropped == 1);
    REQUIRE(!ores_actor_i64_heap_trace_snapshot(heap, records, 7, &summary));
    ores_actor_i64_heap_retire(heap);
    REQUIRE(ores_actor_i64_heap_try_destroy(heap));
}

static void test_overflow_and_different_actor_identities(void) {
    OresActorI64Heap* heap = ores_actor_i64_heap_open(9001, 22, 2);
    REQUIRE(ores_actor_i64_heap_trace_enable(heap, 1));
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 9001, 22);
    OresActorSlot slot = ores_actor_i64_heap_new(heap, turn, 11);
    REQUIRE(ores_actor_i64_heap_read(heap, turn, slot) == 11);
    ores_actor_i64_heap_write(heap, turn, slot, 12);
    ores_actor_i64_heap_leave(heap, turn);
    OresActorHeapAccessEvent event;
    OresActorHeapAccessSummary summary;
    REQUIRE(!ores_actor_i64_heap_trace_snapshot(heap, &event, 1, &summary));
    REQUIRE(summary.stored == 1 && summary.dropped == 2);
    REQUIRE(event.actor_id == 9001 && event.incarnation == 22);
    REQUIRE(event.kind == ORES_ACTOR_HEAP_ACCESS_ALLOC);
    ores_actor_i64_heap_retire(heap);
    REQUIRE(ores_actor_i64_heap_try_destroy(heap));
}

int main(void) {
    test_bounded_successful_accesses();
    test_overflow_and_different_actor_identities();
    puts("ACTOR_HEAP_ACCESS_EVIDENCE|scope=native_i64_host_only|result=PASS");
    return 0;
}
