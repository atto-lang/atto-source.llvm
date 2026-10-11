#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOwnedI64Region.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#define WORKERS 8
#define ITERATIONS 1000
#define REQUIRE(test) do { if (!(test)) abort(); } while (0)

typedef struct {
    OresOwnedI64Region* region;
    uint64_t* seen;
    int worker;
} Worker;

static void* run(void* userdata) {
    Worker* const work = (Worker*)userdata;
    for (int i = 0; i < ITERATIONS; ++i) {
        const int64_t payload = ((int64_t)work->worker << 32) | i;
        const uint64_t handle = oreslang_owned_i64_region_new_handle(work->region, payload);
        REQUIRE(handle != 0);
        REQUIRE(oreslang_owned_i64_region_read_handle(work->region, handle) == payload);
        work->seen[i] = handle;
        oreslang_owned_i64_region_drop_handle(work->region, handle);
    }
    return NULL;
}
static int compare_handles(const void* lhs, const void* rhs) {
    const uint64_t a = *(const uint64_t*)lhs;
    const uint64_t b = *(const uint64_t*)rhs;
    return (a > b) - (a < b);
}
int main(void) {
    OresOwnedI64Region* region = oreslang_owned_i64_region_open_handles(WORKERS);
    uint64_t tokens[WORKERS * ITERATIONS];
    Worker workers[WORKERS];
    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; ++i) {
        workers[i].region = region;
        workers[i].worker = i;
        workers[i].seen = tokens + (i * ITERATIONS);
        REQUIRE(pthread_create(&threads[i], NULL, run, &workers[i]) == 0);
    }
    for (int i = 0; i < WORKERS; ++i)
        REQUIRE(pthread_join(threads[i], NULL) == 0);
    qsort(tokens, WORKERS * ITERATIONS, sizeof(uint64_t), compare_handles);
    for (int i = 1; i < WORKERS * ITERATIONS; ++i)
        REQUIRE(tokens[i] != tokens[i - 1]);
    oreslang_owned_i64_region_close(region);
    return 0;
}
