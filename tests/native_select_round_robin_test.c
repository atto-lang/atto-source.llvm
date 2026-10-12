#include "oreslang/runtime/NativeSelectRoundRobin.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int reference_pick(uint64_t ready, unsigned count, unsigned *cursor) {
    if (!cursor || count == 0 || count > 64 || *cursor >= count) return -1;
    for (unsigned delta = 0; delta < count; delta++) {
        unsigned index = (*cursor + delta) % count;
        if ((ready & (UINT64_C(1) << index)) != 0) {
            *cursor = (index + 1 == count) ? 0 : index + 1;
            return (int)index;
        }
    }
    return -1;
}

int main(void) {
    unsigned cases = 0;
    for (unsigned n = 1; n <= 9; n++) {
        uint64_t limit = UINT64_C(1) << n;
        for (uint64_t mask = 0; mask < limit; mask++) {
            for (unsigned start = 0; start < n; start++) {
                unsigned actual_cursor = start;
                unsigned model_cursor = start;
                int actual = ores_native_select_fair_pick(
                        mask, n, &actual_cursor);
                int model = reference_pick(mask, n, &model_cursor);
                assert(actual == model);
                assert(actual_cursor == model_cursor);
                cases++;
            }
        }
    }

    /* Boundary cases: arm 63, cursor 63, all 64 valid bits. */
    unsigned cursor = 63;
    assert(ores_native_select_fair_pick(
            UINT64_C(1) << 63, 64, &cursor) == 63);
    assert(cursor == 0);
    cursor = 63;
    assert(ores_native_select_fair_pick(
            UINT64_C(1) | (UINT64_C(1) << 63), 64, &cursor) == 63);
    assert(cursor == 0);
    assert(ores_native_select_fair_pick(
            UINT64_C(1) | (UINT64_C(1) << 63), 64, &cursor) == 0);
    assert(cursor == 1);
    cursor = 64;
    assert(ores_native_select_fair_pick(UINT64_MAX, 64, &cursor) == -1);
    assert(cursor == 64);
    cursor = 0;
    assert(ores_native_select_fair_pick(UINT64_MAX, 0, &cursor) == -1);
    assert(ores_native_select_fair_pick(UINT64_MAX, 65, &cursor) == -1);
    assert(ores_native_select_fair_pick(UINT64_MAX, 3, NULL) == -1);
    assert(ores_native_select_fair_pick(0, 64, &cursor) == -1);
    assert(cursor == 0);
    /* Ignore out-of-range bits, do not accidentally activate an arm. */
    assert(ores_native_select_fair_pick(UINT64_C(1) << 63, 3, &cursor) == -1);

    for (unsigned n = 1; n <= 64; n++) {
        uint64_t mask = n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1;
        unsigned frequencies[64] = {0};
        cursor = 0;
        for (unsigned turn = 0; turn < n * 100; turn++) {
            int chosen = ores_native_select_fair_pick(mask, n, &cursor);
            assert(chosen == (int)(turn % n));
            frequencies[chosen]++;
        }
        for (unsigned arm = 0; arm < n; arm++) {
            assert(frequencies[arm] == 100);
        }
    }
    printf("NATIVE_SELECT_FAIR_PASS exhaustive=%u max_arms=64 100_turns_each=1\n", cases);
    return 0;
}
