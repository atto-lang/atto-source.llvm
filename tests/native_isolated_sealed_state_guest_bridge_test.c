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
    require(ores_actor_i64_heap_trace_enable(heap, 16));
    OresActorTurn turn = ores_actor_i64_heap_enter(heap, 912, 1);
    const uint64_t actor_domain = turn.domain;
    const uint64_t init_epoch = turn.epoch;
    oreslang_actor_Counter_init(heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(heap, turn);

    turn = ores_actor_i64_heap_enter(heap, 912, 1);
    require(turn.domain == actor_domain && turn.epoch == init_epoch + 1);
    const uint64_t first_epoch = turn.epoch;
    const int64_t first = oreslang_actor_Counter_on_message(
        heap, turn.domain, turn.epoch, 1);
    ores_actor_i64_heap_leave(heap, turn);
    turn = ores_actor_i64_heap_enter(heap, 912, 1);
    require(turn.domain == actor_domain && turn.epoch == first_epoch + 1);
    const uint64_t second_epoch = turn.epoch;
    const int64_t second = oreslang_actor_Counter_on_message(
        heap, turn.domain, turn.epoch, 2);
    ores_actor_i64_heap_leave(heap, turn);

    require(first == (writing ? 1 : 8));
    require(second == (writing ? 3 : 9));

    /* A zero-work actor exit cannot satisfy this. Only the real guest handler
     * can generate the checked field read/write trace under active turns.
     * Snapshot is complete (no dropped events) before memory is retired. */
    OresActorHeapAccessEvent events[16] = {0};
    OresActorHeapAccessSummary summary = {0};
    require(ores_actor_i64_heap_trace_snapshot(
        heap, events, 16, &summary));
    const uint32_t write_kinds[] = {
        ORES_ACTOR_HEAP_ACCESS_FIELD_INIT,
        ORES_ACTOR_HEAP_ACCESS_FIELD_READ,
        ORES_ACTOR_HEAP_ACCESS_FIELD_WRITE,
        ORES_ACTOR_HEAP_ACCESS_FIELD_READ,
        ORES_ACTOR_HEAP_ACCESS_FIELD_WRITE
    };
    const uint32_t read_kinds[] = {
        ORES_ACTOR_HEAP_ACCESS_FIELD_INIT,
        ORES_ACTOR_HEAP_ACCESS_FIELD_READ,
        ORES_ACTOR_HEAP_ACCESS_FIELD_READ
    };
    const uint32_t* expected_kinds = writing ? write_kinds : read_kinds;
    const uint32_t expected_count = writing ? 5u : 3u;
    require(summary.enabled == 1 && summary.dropped == 0
        && summary.stored == expected_count && summary.capacity == 16);
    for (uint32_t i = 0; i < expected_count; ++i) {
        const OresActorHeapAccessEvent* e = &events[i];
        require(e->kind == expected_kinds[i]);
        require(e->actor_id == 912 && e->incarnation == 1
            && e->domain == actor_domain && e->slot_index == 0
            && e->slot_generation == 1);
        const uint64_t expected_epoch = i == 0 ? init_epoch
            : (writing ? (i <= 2 ? first_epoch : second_epoch)
                       : (i == 1 ? first_epoch : second_epoch));
        require(e->turn_epoch == expected_epoch);
    }
    ores_actor_i64_heap_retire(heap);
    require(ores_actor_i64_heap_try_destroy(heap));
    puts(writing ? "COMPILED_SEALED_WRITE_GUEST_PASS"
                 : "COMPILED_SEALED_READ_GUEST_PASS");
    return 0;
}
