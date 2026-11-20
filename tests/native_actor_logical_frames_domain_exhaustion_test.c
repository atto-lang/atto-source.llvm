/* Host-only boundary regression: compile the real implementation directly
 * so the private counter's unreachable UINT64_MAX edge can be exercised
 * without adding any production test setter or guest-accessible API.
 */
#include "../src/main/cpp/oreslang/runtime/NativeActorLogicalFramesV1.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "logical domain exhaustion failure:%d: %s\\n", \
                __LINE__, #condition); \
        abort(); \
    } \
} while (0)

enum { WORKERS = 8 };
typedef struct {
    OresNativeActorLogicalFramesV1* stack;
    uint64_t domain;
} DomainAttempt;

static void* open_near_exhaustion(void* arg) {
    DomainAttempt* a = (DomainAttempt*)arg;
    a->stack = ores_logical_frames_v1_open(7, 1, 1, 1, 16);
    if (a->stack) {
        OresLogicalTurnV1 token = {0};
        CHECK(ores_logical_frames_v1_begin(a->stack, 7, 1, &token));
        a->domain = token.domain;
        CHECK(ores_logical_frames_v1_end(a->stack, token));
    }
    return NULL;
}

int main(void) {
    /* This test executable alone controls its private static counter;
     * production has no setter and always begins at 1.
     */
    atomic_store_explicit(&next_domain, UINT64_MAX - UINT64_C(2),
                          memory_order_relaxed);
    pthread_t threads[WORKERS];
    DomainAttempt attempts[WORKERS] = {{0}};
    for (size_t i = 0; i < WORKERS; ++i)
        CHECK(pthread_create(&threads[i], NULL,
                             open_near_exhaustion, &attempts[i]) == 0);
    for (size_t i = 0; i < WORKERS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);

    size_t admitted = 0;
    int saw_last_minus_one = 0, saw_last_minus_two = 0;
    for (size_t i = 0; i < WORKERS; ++i) {
        if (!attempts[i].stack) {
            CHECK(attempts[i].domain == 0);
            continue;
        }
        admitted++;
        if (attempts[i].domain == UINT64_MAX - UINT64_C(1))
            saw_last_minus_one++;
        else if (attempts[i].domain == UINT64_MAX - UINT64_C(2))
            saw_last_minus_two++;
        else CHECK(0);
        CHECK(ores_logical_frames_v1_close(attempts[i].stack));
    }
    CHECK(admitted == 2);
    CHECK(saw_last_minus_one == 1 && saw_last_minus_two == 1);
    CHECK(atomic_load_explicit(&next_domain, memory_order_relaxed) == UINT64_MAX);
    for (unsigned i = 0; i < 16; ++i)
        CHECK(ores_logical_frames_v1_open(7, 1, 1, 1, 16) == NULL);
    CHECK(atomic_load_explicit(&next_domain, memory_order_relaxed) == UINT64_MAX);

    puts("logical-frame domains: concurrent unique IDs; permanent exhaustion PASS");
    return 0;
}
