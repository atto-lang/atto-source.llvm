#include "oreslang/runtime/ArcControlV1.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

struct Evidence {
    _Atomic int destroyed;
    _Atomic int reclaimed;
    _Atomic int gate_entered;
    _Atomic int gate_open;
};

static void destroy(void* context, void* payload) {
    struct Evidence* e = context;
    assert(payload != NULL);
    atomic_fetch_add(&e->destroyed, 1);
    atomic_store(&e->gate_entered, 1);
    while (!atomic_load(&e->gate_open)) sched_yield();
}
static void reclaim(void* context, OresArcControlV1* block) {
    struct Evidence* e = context;
    assert(block != NULL);
    assert(atomic_load(&e->destroyed) == 1);
    atomic_fetch_add(&e->reclaimed, 1);
}
struct Release { OresArcControlV1* cell; uint64_t domain, generation; };
static void* final_release(void* arg) {
    struct Release* r = arg;
    assert(ores_arc_v1_release_strong(r->cell, r->domain, r->generation));
    return NULL;
}

static void basic_and_barrier(void) {
    struct Evidence e = {0};
    OresArcControlV1 cell = ORES_ARC_CONTROL_V1_ZERO_INIT;
    int payload = 42;
    assert(!ores_arc_v1_init(&cell, 0, 1, &payload, &e, destroy, reclaim));
    assert(ores_arc_v1_init(&cell, 3, 7, &payload, &e, destroy, reclaim));
    assert(!ores_arc_v1_init(&cell, 3, 7, &payload, &e, destroy, reclaim));
    assert(!ores_arc_v1_downgrade(&cell, 4, 7));
    assert(!ores_arc_v1_clone_strong(&cell, 3, 6));
    assert(!ores_arc_v1_clone_weak(&cell, 3, 7)); /* sentinel is not public */
    assert(!ores_arc_v1_release_weak(&cell, 3, 7));
    assert(ores_arc_v1_downgrade(&cell, 3, 7));
    assert(ores_arc_v1_clone_weak(&cell, 3, 7));
    assert(ores_arc_v1_clone_strong(&cell, 3, 7));
    assert(ores_arc_v1_release_strong(&cell, 3, 7));
    struct Release r = { &cell, 3, 7 };
    pthread_t thread;
    assert(pthread_create(&thread, NULL, final_release, &r) == 0);
    while (!atomic_load(&e.gate_entered)) sched_yield();
    assert(atomic_load(&e.destroyed) == 1);
    assert(atomic_load(&e.reclaimed) == 0);
    assert(!ores_arc_v1_upgrade(&cell, 3, 7));
    assert(!ores_arc_v1_clone_strong(&cell, 3, 7));
    assert(ores_arc_v1_release_weak(&cell, 3, 7));
    assert(atomic_load(&e.reclaimed) == 0); /* sentinel still alive */
    atomic_store(&e.gate_open, 1);
    assert(pthread_join(thread, NULL) == 0);
    assert(atomic_load(&e.reclaimed) == 0); /* last external weak alive */
    assert(ores_arc_v1_clone_weak(&cell, 3, 7)); /* clone real weak after drop */
    assert(ores_arc_v1_release_weak(&cell, 3, 7));
    assert(atomic_load(&e.reclaimed) == 0);
    assert(ores_arc_v1_release_weak(&cell, 3, 7));
    assert(atomic_load(&e.reclaimed) == 1);
}

struct Worker {
    OresArcControlV1* cell;
    _Atomic int* start;
    _Atomic int upgrades;
};
static void* race_worker(void* arg) {
    struct Worker* w = arg;
    while (!atomic_load(w->start)) sched_yield();
    for (int i = 0; i < 10000; i++) {
        if (ores_arc_v1_upgrade(w->cell, 9, 10)) {
            atomic_fetch_add(&w->upgrades, 1);
            assert(ores_arc_v1_release_strong(w->cell, 9, 10));
        }
    }
    assert(ores_arc_v1_release_weak(w->cell, 9, 10));
    return NULL;
}
static void upgrade_release_race(void) {
    struct Evidence e = {0};
    atomic_store(&e.gate_open, 1);
    OresArcControlV1 cell = ORES_ARC_CONTROL_V1_ZERO_INIT;
    int payload = 23;
    assert(ores_arc_v1_init(&cell, 9, 10, &payload, &e, destroy, reclaim));
    enum { N = 8 };
    struct Worker w[N] = {0};
    pthread_t threads[N];
    _Atomic int start = 0;
    for (int i = 0; i < N; ++i) {
        assert(ores_arc_v1_downgrade(&cell, 9, 10));
        w[i].cell = &cell;
        w[i].start = &start;
        assert(pthread_create(&threads[i], NULL, race_worker, &w[i]) == 0);
    }
    atomic_store(&start, 1);
    assert(ores_arc_v1_release_strong(&cell, 9, 10));
    for (int i = 0; i < N; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(atomic_load(&e.destroyed) == 1);
    assert(atomic_load(&e.reclaimed) == 1);
}

static void invalid_generation(void) {
    struct Evidence e = {0};
    atomic_store(&e.gate_open, 1);
    OresArcControlV1 cell = ORES_ARC_CONTROL_V1_ZERO_INIT;
    int payload = 11;
    assert(ores_arc_v1_init(&cell, 5, 17, &payload, &e, destroy, reclaim));
    assert(ores_arc_v1_downgrade(&cell, 5, 17));
    assert(!ores_arc_v1_upgrade(&cell, 5, 18));
    assert(!ores_arc_v1_upgrade(&cell, 6, 17));
    assert(!ores_arc_v1_release_strong(&cell, 6, 17));
    assert(ores_arc_v1_release_strong(&cell, 5, 17));
    assert(!ores_arc_v1_upgrade(&cell, 5, 17));
    assert(ores_arc_v1_release_weak(&cell, 5, 17));
    assert(atomic_load(&e.destroyed) == 1);
    assert(atomic_load(&e.reclaimed) == 1);
}

/* A host allocator supplies the cell and payload. The runtime itself uses
 * no malloc/free: it calls the owner-provided callbacks at the exact edges. */
static void heap_destroy(void* context, void* payload) {
    struct Evidence* e = context;
    assert(payload != NULL);
    atomic_fetch_add(&e->destroyed, 1);
    free(payload);
}
static void heap_reclaim(void* context, OresArcControlV1* control) {
    struct Evidence* e = context;
    assert(atomic_load(&e->destroyed) == 1);
    atomic_fetch_add(&e->reclaimed, 1);
    free(control);
}
static void physical_owner_release(void) {
    struct Evidence e = {0};
    OresArcControlV1* block = calloc(1, sizeof(*block));
    int* payload = malloc(sizeof(*payload));
    assert(block && payload);
    *payload = 42;
    assert(ores_arc_v1_init(block, 2, 99, payload, &e,
                            heap_destroy, heap_reclaim));
    assert(ores_arc_v1_downgrade(block, 2, 99));
    assert(ores_arc_v1_release_strong(block, 2, 99));
    assert(atomic_load(&e.destroyed) == 1);
    assert(atomic_load(&e.reclaimed) == 0);
    assert(!ores_arc_v1_upgrade(block, 2, 99));
    assert(ores_arc_v1_release_weak(block, 2, 99));
    /* No more block accesses: it was physically freed by the final weak. */
    assert(atomic_load(&e.reclaimed) == 1);
}

int main(void) {
    basic_and_barrier();
    for (int i = 0; i < 100; i++) upgrade_release_race();
    invalid_generation();
    physical_owner_release();
    puts("ATTO_ARC_ATOMIC_CONTROL_HOST_PASS");
    return 0;
}
