#ifndef ORES_NATIVE_ACTOR_DOMAIN_ID_H
#define ORES_NATIVE_ACTOR_DOMAIN_ID_H

#include <stdint.h>
#include <stdatomic.h>

/* Disjoint numerical domains for native byte arenas and scalar heaps.
 * Each partition retains its own process-lifetime atomic ordinal counter.
 * This does not protect against reloading a runtime image with reset state
 * or using an invalidated raw heap pointer after destruction.
 */
#define ORES_NATIVE_DOMAIN_PARTITION_BIT (UINT64_C(1) << 63)
#define ORES_NATIVE_DOMAIN_SCALAR UINT64_C(0)
#define ORES_NATIVE_DOMAIN_ARENA ORES_NATIVE_DOMAIN_PARTITION_BIT

/* Ordinals [1, 2^63 - 1] are valid; 2^63 is the terminal sentinel.
 * Only the two specified partition tags are accepted. Fail closed on zero,
 * exhausted counters and invalid tags without ever wrapping a counter.
 */
static inline uint64_t ores_native_actor_domain_reserve(
        _Atomic(uint64_t)* next, uint64_t partition) {
    if (partition != ORES_NATIVE_DOMAIN_SCALAR &&
        partition != ORES_NATIVE_DOMAIN_ARENA) return 0;
    uint64_t current = atomic_load_explicit(next, memory_order_relaxed);
    while (current != 0 && current < ORES_NATIVE_DOMAIN_PARTITION_BIT) {
        if (atomic_compare_exchange_weak_explicit(
                next, &current, current + 1,
                memory_order_relaxed, memory_order_relaxed))
            return current | partition;
    }
    return 0;
}

#endif /* ORES_NATIVE_ACTOR_DOMAIN_ID_H */
