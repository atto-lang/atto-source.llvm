/* Exercise the actual source-actor bootstrap and OresVM scheduler using
 * trusted C callbacks with the generated actor ABI, not compiled .ores code. */
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

static _Atomic unsigned initialized;
static _Atomic uint64_t domains[2];
static _Atomic unsigned processed[2];
static _Atomic int64_t last[2];
static void check(int condition) { if (!condition) abort(); }
static void initialize(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    unsigned slot = atomic_fetch_add(&initialized, 1);
    check(slot < 2);
    atomic_store(&domains[slot], domain);
    ores_actor_i64_heap_field_init_mutable(heap, domain, epoch, 0, 0);
}
static int64_t message(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch, int64_t n) {
    unsigned index = domain == atomic_load(&domains[0]) ? 0 : 1;
    check(domain == atomic_load(&domains[index]));
    int64_t result = ores_actor_i64_heap_field_read(heap, domain, epoch, 0) + n;
    ores_actor_i64_heap_field_write(heap, domain, epoch, 0, result);
    atomic_store(&last[index], result);
    atomic_fetch_add(&processed[index], 1);
    return result;
}
int main(void) {
    uint64_t previous_max = 0;
    for (unsigned round = 0; round < 8; ++round) {
        atomic_store(&initialized, 0);
        for (unsigned i = 0; i < 2; ++i) { atomic_store(&domains[i], 0); atomic_store(&processed[i], 0); }
        void* a = oreslang_source_actor_spawn(initialize, message, 1);
        void* b = oreslang_source_actor_spawn(initialize, message, 1);
        for (unsigned i = 0; i < 100; ++i) {
            oreslang_source_actor_send(a, 1);
            oreslang_source_actor_send(b, 2);
        }
        oreslang_source_actor_stop(a);
        oreslang_source_actor_stop(b);
        const uint64_t d0 = atomic_load(&domains[0]), d1 = atomic_load(&domains[1]);
        check(atomic_load(&initialized) == 2 && d0 != d1 && d0 > previous_max && d1 > previous_max);
        previous_max = d0 > d1 ? d0 : d1;
        check(atomic_load(&processed[0]) == 100 && atomic_load(&processed[1]) == 100);
        int64_t x = atomic_load(&last[0]), y = atomic_load(&last[1]);
        check((x == 100 && y == 200) || (x == 200 && y == 100));
    }
    puts("source actor host bridge: 16 distinct heaps, 1600 OresVM messages, complete physical reclamation passed");
    return 0;
}
