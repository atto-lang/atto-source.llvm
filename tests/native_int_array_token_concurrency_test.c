#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

/* Parallel independent regions are allowed, but a region must never be
 * shared between workers. Global tokens must remain strictly nonrepeating
 * even across slot reuse and concurrent region creation. */
enum { THREADS = 8, ROUNDS = 1000, PER_WORKER = ROUNDS * 2 };
typedef struct {
    OresNativeIntArrayHandle tokens[PER_WORKER];
    unsigned worker;
} Worker;

static void demand(int condition) { if (!condition) abort(); }

static void* work(void* opaque) {
    Worker* w = (Worker*)opaque;
    OresNativeIntArrayRegion* r = oreslang_int_array_region_open();
    for (unsigned i = 0; i < ROUNDS; ++i) {
        int64_t value = (int64_t)(w->worker * ROUNDS + i);
        OresNativeIntArrayHandle first = oreslang_int_array_new(r);
        OresNativeIntArrayHandle second = oreslang_int_array_new(r);
        demand(first != 0 && second != 0 && first != second);
        oreslang_int_array_add(r, first, value);
        oreslang_int_array_add(r, second, -value);
        demand(oreslang_int_array_get(r, first, 0) == value);
        demand(oreslang_int_array_get(r, second, 0) == -value);
        w->tokens[i * 2] = first;
        w->tokens[i * 2 + 1] = second;
        oreslang_int_array_drop(r, second);
        oreslang_int_array_drop(r, first);
    }
    oreslang_int_array_region_close(r);
    return NULL;
}

static int compare_tokens(const void* lhs, const void* rhs) {
    uint64_t a = *(const uint64_t*)lhs;
    uint64_t b = *(const uint64_t*)rhs;
    return (a > b) - (a < b);
}

int main(void) {
    Worker* workers = (Worker*)calloc(THREADS, sizeof(*workers));
    OresNativeIntArrayHandle* all =
        (OresNativeIntArrayHandle*)malloc(THREADS * PER_WORKER * sizeof(*all));
    demand(workers != NULL && all != NULL);
    pthread_t ids[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) {
        workers[i].worker = i;
        demand(pthread_create(&ids[i], NULL, work, &workers[i]) == 0);
    }
    for (unsigned i = 0; i < THREADS; ++i)
        demand(pthread_join(ids[i], NULL) == 0);
    for (unsigned i = 0; i < THREADS; ++i)
        for (unsigned j = 0; j < PER_WORKER; ++j)
            all[i * PER_WORKER + j] = workers[i].tokens[j];
    qsort(all, THREADS * PER_WORKER, sizeof(*all), compare_tokens);
    for (size_t i = 0; i < THREADS * PER_WORKER; ++i) {
        demand(all[i] != 0 && all[i] != UINT64_MAX);
        if (i != 0) demand(all[i] > all[i - 1]);
    }
    free(all);
    free(workers);
    return 0;
}
