#include "oreslang/runtime/NativeIntArrayRegion.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Repeatable model-based fuzzing without undefined signed arithmetic.
 * Tests real native ABI calls on independently owned arrays, not a mock.
 */
enum { OWNERS = 12, STEPS = 18000, MAX_MODEL = 256 };
typedef struct {
    OresNativeIntArrayHandle handle;
    int64_t values[MAX_MODEL];
    uint32_t size;
    int alive;
} Model;

static uint64_t random_state;
static uint64_t next_random(void) {
    uint64_t x = random_state;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    random_state = x;
    return x * UINT64_C(2685821657736338717);
}
static void require_condition(int ok, const char* what, uint64_t step, int owner) {
    if (!ok) {
        fprintf(stderr, "array model failure: %s step=%llu owner=%d seed=%llu\n",
                what, (unsigned long long)step, owner,
                (unsigned long long)random_state);
        abort();
    }
}
static void check_model(OresNativeIntArrayRegion* r, Model* m,
                        uint64_t step, int owner) {
    if (!m->alive) return;
    require_condition(oreslang_int_array_length(r, m->handle) == m->size,
                      "length mismatch", step, owner);
    for (uint32_t j = 0; j < m->size; ++j) {
        require_condition(oreslang_int_array_get(r, m->handle, j) == m->values[j],
                          "element mismatch", step, owner);
    }
}
static void scenario(uint64_t seed) {
    random_state = seed;
    Model owners[OWNERS] = {0};
    OresNativeIntArrayRegion* region = oreslang_int_array_region_open();
    for (uint64_t step = 0; step < STEPS; ++step) {
        uint64_t draw = next_random();
        int owner = (int)(draw % OWNERS);
        Model* m = &owners[owner];
        uint32_t kind = (uint32_t)((draw >> 10) % 9);
        if (!m->alive) {
            m->handle = oreslang_int_array_new(region);
            m->alive = 1;
            m->size = 0;
        } else if (kind == 0) {
            OresNativeIntArrayHandle old = m->handle;
            oreslang_int_array_drop(region, old);
            m->handle = oreslang_int_array_new(region);
            require_condition(m->handle != old, "token reused", step, owner);
            m->size = 0;
        } else if (kind <= 3 && m->size < MAX_MODEL) {
            int64_t v = (int64_t)next_random();
            if (kind == 1) v = INT64_MIN;
            if (kind == 2) v = INT64_MAX;
            oreslang_int_array_add(region, m->handle, v);
            m->values[m->size++] = v;
        } else if (kind <= 5 && m->size > 0) {
            uint32_t at = (uint32_t)(next_random() % m->size);
            int64_t v = (int64_t)next_random();
            oreslang_int_array_set(region, m->handle, at, v);
            m->values[at] = v;
        } else if (kind == 6 && m->size > 0) {
            uint32_t at = (uint32_t)(next_random() % m->size);
            require_condition(oreslang_int_array_get(region, m->handle, at) ==
                              m->values[at], "random get", step, owner);
        } else if (kind == 7) {
            /* Allocation-free no-op reserve, even with requested capacity 0. */
            oreslang_int_array_reserve(region, m->handle, 0);
        } else if (kind == 8 && m->size < 220) {
            oreslang_int_array_reserve(region, m->handle, 256);
        }
        /* Full scan catches accidental mutation of OTHER owners as well. */
        if (step % 101 == 0) {
            for (int i = 0; i < OWNERS; ++i)
                check_model(region, &owners[i], step, i);
        }
    }
    for (int i = 0; i < OWNERS; ++i) {
        check_model(region, &owners[i], STEPS, i);
        if (owners[i].alive) oreslang_int_array_drop(region, owners[i].handle);
    }
    oreslang_int_array_region_close(region);
}
int main(void) {
    const uint64_t seeds[] = {
        UINT64_C(1), UINT64_C(0x9e3779b97f4a7c15),
        UINT64_C(0x123456789abcdef0), UINT64_C(0xdeadbeefcafefeed)
    };
    for (size_t i = 0; i < sizeof(seeds)/sizeof(seeds[0]); ++i)
        scenario(seeds[i]);
    puts("PASS: native Array<int> model oracle (4 seeds x 18000 operations)");
    return 0;
}
