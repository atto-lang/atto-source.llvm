#include "oreslang/runtime/NativeActorDomainId.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define WORKERS 12
#define ATTEMPTS 128
#define LAST_IDS 8
#define ROUNDS 32
#define MAX_ISSUED (WORKERS * ATTEMPTS)

static _Atomic(uint64_t) shared_next;
static _Atomic(unsigned) issued_count;
static uint64_t issued[MAX_ISSUED];
static unsigned expected_count;
static uint64_t expected_partition;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_condition = PTHREAD_COND_INITIALIZER;
static pthread_cond_t start_condition = PTHREAD_COND_INITIALIZER;
static unsigned ready;
static int start;

static void fail(const char* reason) {
    fprintf(stderr, "FAIL native actor ID exhaustion: %s\n", reason);
    exit(1);
}

static void* contender(void* unused) {
    (void)unused;
    if (pthread_mutex_lock(&gate_lock)) fail("gate lock failed");
    ++ready;
    if (pthread_cond_signal(&ready_condition)) fail("ready signal failed");
    while (!start) {
        if (pthread_cond_wait(&start_condition, &gate_lock)) fail("start wait failed");
    }
    if (pthread_mutex_unlock(&gate_lock)) fail("gate unlock failed");
    for (unsigned attempt = 0; attempt < ATTEMPTS; ++attempt) {
        const uint64_t id = ores_native_actor_domain_reserve(&shared_next, expected_partition);
        if (id == 0) continue;
        const unsigned index = atomic_fetch_add_explicit(
            &issued_count, 1, memory_order_relaxed);
        if (index >= expected_count) fail("issued more identities than expected");
        issued[index] = id;
    }
    return NULL;
}

static int compare_ids(const void* left, const void* right) {
    const uint64_t a = *(const uint64_t*)left;
    const uint64_t b = *(const uint64_t*)right;
    return (a > b) - (a < b);
}

static void run_case(uint64_t partition, uint64_t first, unsigned count, uint64_t final) {
    atomic_store_explicit(&shared_next, first, memory_order_relaxed);
    atomic_store_explicit(&issued_count, 0, memory_order_relaxed);
    expected_count = count;
    expected_partition = partition;
    ready = 0;
    start = 0;

    pthread_t threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) {
        if (pthread_create(&threads[i], NULL, contender, NULL))
            fail("failed to create contention thread");
    }
    /* Hold every worker at the gate before releasing them together. This
     * increases contention opportunities; it is not a scheduler guarantee
     * that individual compare-exchanges overlap. */
    if (pthread_mutex_lock(&gate_lock)) fail("gate lock failed");
    while (ready != WORKERS) {
        if (pthread_cond_wait(&ready_condition, &gate_lock)) fail("ready wait failed");
    }
    start = 1;
    if (pthread_cond_broadcast(&start_condition)) fail("start broadcast failed");
    if (pthread_mutex_unlock(&gate_lock)) fail("gate unlock failed");
    for (unsigned i = 0; i < WORKERS; ++i) {
        if (pthread_join(threads[i], NULL)) fail("failed to join contender");
    }
    if (atomic_load_explicit(&issued_count, memory_order_relaxed) != count)
        fail("did not allocate the expected number of unique identities");

    qsort(issued, count, sizeof(issued[0]), compare_ids);
    for (unsigned i = 0; i < count; ++i) {
        if (issued[i] != ((first + i) | partition)) fail("duplicate, missing or invalid identity");
    }

    if (atomic_load_explicit(&shared_next, memory_order_relaxed) != final)
        fail("unexpected final counter or terminal wraparound");
    if (final == ORES_NATIVE_DOMAIN_PARTITION_BIT || final == UINT64_MAX || final == 0) {
        for (unsigned i = 0; i < 1024; ++i) {
            if (ores_native_actor_domain_reserve(&shared_next, expected_partition))
                fail("terminal state reissued a previously used domain");
        }
        if (atomic_load_explicit(&shared_next, memory_order_relaxed) != final)
            fail("exhausted calls changed terminal state");
    }
}

int main(void) {
    const uint64_t limit = ORES_NATIVE_DOMAIN_PARTITION_BIT;
    const uint64_t partitions[] = {ORES_NATIVE_DOMAIN_SCALAR,
                                   ORES_NATIVE_DOMAIN_ARENA};
    for (unsigned kind = 0; kind < 2; ++kind) {
        const uint64_t partition = partitions[kind];
        for (unsigned round = 0; round < ROUNDS; ++round)
            run_case(partition, limit - LAST_IDS, LAST_IDS, limit);
        run_case(partition, 1, MAX_ISSUED, 1 + MAX_ISSUED);
        run_case(partition, limit - 1, 1, limit);
        run_case(partition, limit, 0, limit);
        run_case(partition, UINT64_MAX, 0, UINT64_MAX);
        run_case(partition, 0, 0, 0);
    }

    _Atomic(uint64_t) arena_next = 1, scalar_next = 1;
    uint64_t arena_id = ores_native_actor_domain_reserve(
        &arena_next, ORES_NATIVE_DOMAIN_ARENA);
    uint64_t scalar_id = ores_native_actor_domain_reserve(
        &scalar_next, ORES_NATIVE_DOMAIN_SCALAR);
    if (scalar_id != 1 || arena_id != (limit | 1) || arena_id == scalar_id)
        fail("cross-API numeric collision");
    if (ores_native_actor_domain_reserve(&scalar_next, 2) != 0 ||
        atomic_load_explicit(&scalar_next, memory_order_relaxed) != 2)
        fail("invalid partition advanced counter");

    puts("PASS native actor partition IDs: 64 gated 12-thread terminal rounds, disjoint namespaces, ordinary uniqueness and exhausted/zero rejection");
    return 0;
}
