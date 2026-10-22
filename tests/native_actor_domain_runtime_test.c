/* Host-only smoke across both actual native allocator implementations.
 * Verifies domain partitioning and loans pin reclamation, not guest compiler
 * code generation or the fencing of arbitrary asynchronous callback holders.
 */
#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorDomainId.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CYCLES 256u

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL native actor domain runtime: %s\n", what);
        exit(1);
    }
}

int main(void) {
    for (uint64_t i = 1; i <= CYCLES; ++i) {
        OresActorArena* arena = ores_actor_arena_open(i, 1, 8, 64, 128);
        OresActorI64Heap* heap = ores_actor_i64_heap_open(i, 1, 8);
        OresArenaTurn at = ores_actor_arena_enter(arena, i, 1);
        OresActorTurn ht = ores_actor_i64_heap_enter(heap, i, 1);

        check(at.domain == (ORES_NATIVE_DOMAIN_ARENA | i),
              "byte arena issued unexpected domain");
        check(ht.domain == (ORES_NATIVE_DOMAIN_SCALAR | i),
              "scalar heap issued unexpected domain");
        check(at.domain != ht.domain, "two live APIs issued overlapping domains");

        OresArenaHandle ah = ores_actor_arena_alloc(arena, at, sizeof(i));
        ores_actor_arena_write(arena, at, ah, 0, &i, sizeof(i));
        uint64_t observed = 0;
        ores_actor_arena_read(arena, at, ah, 0, &observed, sizeof(observed));
        check(observed == i, "byte arena round trip failed");

        OresActorSlot sh = ores_actor_i64_heap_new(heap, ht, (int64_t)i);
        check(ores_actor_i64_heap_read(heap, ht, sh) == (int64_t)i,
              "scalar heap round trip failed");

        OresArenaLoan al = ores_actor_arena_borrow(arena, at, ah);
        OresActorLoan hl = ores_actor_i64_heap_borrow(heap, ht, sh);
        check(ores_actor_i64_heap_loan_read(heap, ht, hl) == (int64_t)i,
              "scalar loan read failed");
        observed = 0;
        ores_actor_arena_loan_read(arena, at, al, 0, &observed, sizeof(observed));
        check(observed == i, "arena loan read failed");

        ores_actor_arena_leave(arena, at);
        ores_actor_i64_heap_leave(heap, ht);
        ores_actor_arena_retire(arena);
        ores_actor_i64_heap_retire(heap);

        /* The retired heap is still live until cross-turn loans drain. */
        check(ores_actor_arena_try_destroy(arena) == 0,
              "arena destroyed with outstanding loan");
        check(ores_actor_i64_heap_try_destroy(heap) == 0,
              "scalar heap destroyed with outstanding loan");
        ores_actor_arena_release(arena, al);
        ores_actor_i64_heap_release(heap, hl);
        check(ores_actor_arena_try_destroy(arena) == 1,
              "arena failed to reclaim after loan drain");
        check(ores_actor_i64_heap_try_destroy(heap) == 1,
              "scalar heap failed to reclaim after loan drain");
    }
    puts("PASS 256 paired live native arena/scalar heaps: partitioned IDs, loan pinning, retirement and reclamation");
    return 0;
}
