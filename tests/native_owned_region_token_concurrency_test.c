#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

/* Each region is owned by exactly one thread. This tests GLOBAL token
 * uniqueness under concurrent allocations; it does not claim that a
 * single region supports concurrent read/drop/close or hostile FFI callers. */
enum { THREADS = 8, ROUNDS = 1000, PER_WORKER = ROUNDS * 2 };
typedef struct {
    uint64_t tokens[PER_WORKER];
    unsigned worker;
} Worker;

static void demand(int valid) {
    if (!valid) abort();
}
static void* run_worker(void* opaque) {
    Worker* w = (Worker*)opaque;
    OresOwnedI64Region* region = oreslang_owned_i64_region_open(2);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        int64_t a = (int64_t)(w->worker * ROUNDS + i);
        OresOwnedI64Handle first = oreslang_owned_i64_region_new(region, a);
        OresOwnedI64Handle second = oreslang_owned_i64_region_new(region, a + 1);
        demand(first != 0 && second != 0 && first != second);
        demand(oreslang_owned_i64_region_read(region, first) == a);
        demand(oreslang_owned_i64_region_read(region, second) == a + 1);
        w->tokens[2 * i] = first;
        w->tokens[2 * i + 1] = second;
        oreslang_owned_i64_region_drop(region, first);
        oreslang_owned_i64_region_drop(region, second);
    }
    oreslang_owned_i64_region_close(region);
    return NULL;
}
static int compare_tokens(const void* left, const void* right) {
    uint64_t a = *(const uint64_t*)left;
    uint64_t b = *(const uint64_t*)right;
    return (a > b) - (a < b);
}
int main(void) {
    Worker* workers = (Worker*)calloc(THREADS, sizeof(*workers));
    uint64_t* all = (uint64_t*)malloc(sizeof(*all) * THREADS * PER_WORKER);
    demand(workers && all);
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) {
        workers[i].worker = i;
        demand(pthread_create(&threads[i], NULL, run_worker, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < THREADS; ++i)
        demand(pthread_join(threads[i], NULL) == 0);
    for (unsigned i = 0; i < THREADS; ++i)
        for (unsigned j = 0; j < PER_WORKER; ++j)
            all[i * PER_WORKER + j] = workers[i].tokens[j];
    qsort(all, THREADS * PER_WORKER, sizeof(*all), compare_tokens);
    for (size_t i = 0; i < THREADS * PER_WORKER; ++i) {
        demand(all[i] != 0 && all[i] != UINT64_MAX);
        if (i) demand(all[i] > all[i - 1]);
    }
    free(all);
    free(workers);
    return 0;
}
