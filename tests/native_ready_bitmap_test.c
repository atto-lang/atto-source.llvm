#include "oreslang/runtime/NativeReadyBitmap.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t seed = UINT32_C(0x9e3779b9);
static uint32_t random32(void) {
    seed ^= seed << 13u;
    seed ^= seed >> 17u;
    seed ^= seed << 5u;
    return seed;
}

static int naive_find(const unsigned char* expected,
                      size_t n, size_t start, size_t* out) {
    for (size_t offset = 0; offset < n; ++offset) {
        size_t at = (start + offset) % n;
        if (expected[at]) { *out = at; return 1; }
    }
    return 0;
}

int main(void) {
    OresNativeReadyBitmap bitmaps[3] = {0};
    unsigned char expected[3][ORES_NATIVE_READY_BITMAP_MAX_ACTORS] = {{0}};
    const size_t sizes[] = {1,2,63,64,65,127,128,129,511,512,513,1023,1024};
    for (size_t k = 0; k < sizeof(sizes)/sizeof(sizes[0]); ++k) {
        const size_t n = sizes[k];
        for (int pass = 0; pass < 3; ++pass) {
            for (size_t i = 0; i < ORES_NATIVE_READY_BITMAP_MAX_ACTORS; ++i) {
                ores_native_ready_bitmap_update(&bitmaps[pass], i, 0);
                expected[pass][i] = 0;
            }
        }
        for (size_t turn = 0; turn < 5000; ++turn) {
            const unsigned pass = random32() % 3u;
            const size_t position = random32() % n;
            const int enabled = (int)(random32() & 1u);
            expected[pass][position] = (unsigned char)enabled;
            ores_native_ready_bitmap_update(&bitmaps[pass], position, enabled);
            const size_t start = random32() % n;
            for (unsigned priority = 0; priority < 3u; ++priority) {
                size_t want = SIZE_MAX, got = SIZE_MAX;
                int observed = ores_native_ready_bitmap_find(&bitmaps[priority], n, start, &got);
                int reference = naive_find(expected[priority], n, start, &want);
                assert(observed == reference);
                assert(!observed || got == want);
            }
        }
    }
    OresNativeReadyBitmap empty = {0};
    size_t result = SIZE_MAX;
    assert(!ores_native_ready_bitmap_find(&empty, 0, 0, &result));
    assert(!ores_native_ready_bitmap_find(&empty, 1025, 0, &result));
    assert(!ores_native_ready_bitmap_find(&empty, 1024, 1024, &result));
    assert(result == SIZE_MAX);
    puts("native_ready_bitmap_test PASS");
    return 0;
}
