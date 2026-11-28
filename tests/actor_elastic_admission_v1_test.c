#include "actor_elastic_admission_v1.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sched.h>

static void workers(atto_elastic_gate_v1 *g, unsigned n) {
    for (unsigned i = 0; i < 8; i++) assert(atto_elastic_set_worker_v1(g, i, 1, i < n) == 1);
}

typedef struct {
    atto_elastic_gate_v1 *gate;
    unsigned lane;
    atomic_uint *started;
    atomic_int *release;
} context;
static void *worker(void *raw) {
    context *c = raw;
    atto_elastic_lease_v1 lease;
    assert(atto_elastic_try_enter_v1(c->gate, 11, 99, c->lane, 1, &lease) == 1);
    atomic_fetch_add(c->started, 1);
    while (!atomic_load(c->release)) sched_yield();
    assert(atto_elastic_release_v1(c->gate, &lease) == 1);
    return NULL;
}
int main(void) {
    atto_elastic_gate_v1 g;
    assert(atto_elastic_init_v1(&g, 11, 99, ATTO_ELASTIC_V1_POOL_SIZE,
        0, ATTO_ELASTIC_AFFINITY_STICKY, 0, 64, 64, 64) == 1);
    workers(&g, 2);
    assert(atto_elastic_capacity_v1(&g) == 2);
    atto_elastic_lease_v1 a, b;
    assert(atto_elastic_try_enter_v1(&g, 11, 99, 0, 1, &a) == 1);
    assert(atto_elastic_try_enter_v1(&g, 11, 99, 1, 1, &b) == 1);
    assert(atto_elastic_try_enter_v1(&g, 11, 99, 0, 1, &a) == 0);
    assert(atto_elastic_release_v1(&g, &a) == 1);
    assert(atto_elastic_release_v1(&g, &b) == 1);
    workers(&g, 8);
    assert(atto_elastic_capacity_v1(&g) == 8);
    pthread_t threads[8]; context contexts[8];
    atomic_uint started = 0; atomic_int release = 0;
    for (unsigned i = 0; i < 8; i++) {
        contexts[i] = (context){&g, i, &started, &release};
        assert(pthread_create(&threads[i], NULL, worker, &contexts[i]) == 0);
    }
    while (atomic_load(&started) < 8) sched_yield();
    assert(atto_elastic_running_v1(&g) == 8);
    atomic_store(&release, 1);
    for (unsigned i = 0; i < 8; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(g.peak == 8 && atto_elastic_running_v1(&g) == 0);
    workers(&g, 8);
    atto_elastic_lease_v1 held[8];
    for (unsigned i = 0; i < 8; i++) assert(atto_elastic_try_enter_v1(&g,11,99,i,1,&held[i]) == 1);
    workers(&g, 1);
    assert(atto_elastic_capacity_v1(&g) == 1);
    assert(atto_elastic_try_enter_v1(&g,11,99,0,1,&a) == 0);
    for (unsigned i = 0; i < 8; i++) assert(atto_elastic_release_v1(&g,&held[i]) == 1);
    assert(atto_elastic_try_enter_v1(&g,11,99,0,1,&a) == 1);
    atto_elastic_stop_v1(&g);
    assert(atto_elastic_try_enter_v1(&g,11,99,0,1,&b) == 0);
    assert(atto_elastic_destroy_v1(&g) == -1);
    assert(atto_elastic_release_v1(&g,&a) == 1);
    assert(atto_elastic_release_v1(&g,&a) == -1);
    assert(atto_elastic_destroy_v1(&g) == 1);

    atto_elastic_gate_v1 pinned;
    assert(atto_elastic_init_v1(&pinned,12,99,1,1,ATTO_ELASTIC_AFFINITY_PINNED,0,4,4,4)==1);
    workers(&pinned, 2);
    assert(atto_elastic_try_enter_v1(&pinned,12,99,0,1,&a)==1);
    assert(atto_elastic_try_enter_v1(&pinned,12,99,1,1,&b)==0);
    assert(atto_elastic_set_worker_v1(&pinned,0,0,0)==1);
    assert(atto_elastic_try_enter_v1(&pinned,12,99,1,1,&b)==0);
    assert(atto_elastic_release_v1(&pinned,&a)==1);
    assert(atto_elastic_try_enter_v1(&pinned,12,99,1,1,&b)==1);
    assert(atto_elastic_try_enter_v1(&pinned,13,99,1,1,&a)==-1);
    assert(atto_elastic_release_v1(&pinned,&b)==1);
    atto_elastic_stop_v1(&pinned);
    assert(atto_elastic_destroy_v1(&pinned)==1);
    puts("native host actor elastic admission tests: PASS (not guest compiler proof)");
    return 0;
}
