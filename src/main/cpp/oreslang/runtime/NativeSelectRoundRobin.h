#ifndef ORESLANG_NATIVE_SELECT_ROUND_ROBIN_H
#define ORESLANG_NATIVE_SELECT_ROUND_ROBIN_H

#include <stdint.h>

/*
 * Pure host-side FAIR arm-selection helper for a single *atomic commit*.
 * No OS syscalls, no allocation, and no mutation when no arm is ready.
 *
 * Caller must serialize observation of ready_mask with removal/commit of the
 * selected operation, and own/fence cursor for this actor-incarnation and
 * select-site. This is NOT LLVM source lowering or a channel waiter registry.
 *
 * Each of 1..64 arms has one unique circular distance, so no random tie
 * breaker is needed. Strict CONTROL priority, default, channel close, and
 * generation cancellation are handled by the future outer arbitration layer.
 */
static inline int ores_native_select_fair_pick(
        uint64_t ready_mask, unsigned arms, unsigned *cursor) {
    if (!cursor || arms == 0 || arms > 64 || *cursor >= arms) return -1;
    /* Shifting by 64 is undefined C: handle 64 separately. */
    uint64_t valid = arms == 64
            ? UINT64_MAX
            : ((UINT64_C(1) << arms) - UINT64_C(1));
    ready_mask &= valid;
    if (!ready_mask) return -1;

    uint64_t suffix = ready_mask & (UINT64_MAX << *cursor);
    uint64_t selected = suffix ? suffix : ready_mask;
    unsigned index = 0;
#if defined(__clang__) || defined(__GNUC__)
    /* selected is checked nonzero above, so ctz is always defined. */
    index = (unsigned)__builtin_ctzll((unsigned long long)selected);
#else
    /* Portable C11 fallback for non-GNU toolchains. */
    while ((selected & UINT64_C(1)) == 0) {
        ++index;
        selected >>= 1;
    }
#endif
    *cursor = index + 1u == arms ? 0u : index + 1u;
    return (int)index;
}

#endif
