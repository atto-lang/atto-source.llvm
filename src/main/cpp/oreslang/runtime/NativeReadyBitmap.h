#ifndef ORESLANG_RUNTIME_NATIVE_READY_BITMAP_H
#define ORESLANG_RUNTIME_NATIVE_READY_BITMAP_H

#include <stddef.h>
#include <stdint.h>

/* Scheduler contract accepts at most 1024 actor slots. These bitsets are
 * embedded in its zero-initialized authority object; no per-message malloc.
 * Every mutation is done under the scheduler mutex. */
#define ORES_NATIVE_READY_BITMAP_MAX_ACTORS 1024u
#define ORES_NATIVE_READY_BITMAP_WORDS (ORES_NATIVE_READY_BITMAP_MAX_ACTORS / 64u)

typedef struct {
    uint64_t words[ORES_NATIVE_READY_BITMAP_WORDS];
} OresNativeReadyBitmap;

static inline void ores_native_ready_bitmap_update(
        OresNativeReadyBitmap* bitmap, size_t index, int enabled) {
    /* Caller already proved index < actor_count <= MAX_ACTORS. */
    const uint64_t bit = UINT64_C(1) << (index & 63u);
    uint64_t* word = &bitmap->words[index >> 6u];
    if (enabled) *word |= bit;
    else *word &= ~bit;
}

static inline unsigned ores_native_ready_bitmap_lowest_bit(uint64_t bits) {
    /* Called only on nonzero words. Portable, bounded and inlineable. */
    unsigned n = 0;
    while (!(bits & UINT64_C(1))) { ++n; bits >>= 1u; }
    return n;
}

/* Return the first set bit starting at 'start', wrapping exactly once. */
static inline int ores_native_ready_bitmap_find(
        const OresNativeReadyBitmap* bitmap, size_t actors, size_t start,
        size_t* out) {
    if (!bitmap || !out || !actors
            || actors > ORES_NATIVE_READY_BITMAP_MAX_ACTORS || start >= actors)
        return 0;
    const size_t first_word = start >> 6u;
    const size_t last_word = (actors - 1u) >> 6u;
    const uint64_t first_bits = bitmap->words[first_word];
    for (size_t word = first_word; word <= last_word; ++word) {
        uint64_t bits = word == first_word ? first_bits : bitmap->words[word];
        if (word == first_word) bits &= UINT64_MAX << (start & 63u);
        if (bits) {
            const size_t index = (word << 6u) + ores_native_ready_bitmap_lowest_bit(bits);
            if (index < actors) { *out = index; return 1; }
        }
    }
    for (size_t word = 0; word < first_word; ++word) {
        const uint64_t bits = bitmap->words[word];
        if (bits) { *out = (word << 6u) + ores_native_ready_bitmap_lowest_bit(bits); return 1; }
    }
    /* Reuse the cached first word instead of rereading a cache line. */
    const uint64_t lower = first_bits
        & ((UINT64_C(1) << (start & 63u)) - UINT64_C(1));
    if (lower) {
        *out = (first_word << 6u) + ores_native_ready_bitmap_lowest_bit(lower);
        return 1;
    }
    return 0;
}

#endif
