#ifndef ORES_NATIVE_ACTOR_DOMAIN_ID_H
#define ORES_NATIVE_ACTOR_DOMAIN_ID_H

#include <stdint.h>
#include <stdatomic.h>

/*
 * Mint lifetime-unique actor domains without wrapping the atomic counter.
 *
 * UINT64_MAX is reserved as the permanently exhausted state. Zero is invalid.
 * Returning zero is a fail-closed admission result; a caller must reject the
 * actor construction without publishing any heap/arena capability.
 *
 * The counter belongs to its host subsystem. Do not use this helper with
 * a fresh counter on every actor incarnation or handle ABA will reappear.
 */
static inline uint64_t ores_native_actor_domain_reserve(
        _Atomic(uint64_t)* next) {
    uint64_t current = atomic_load_explicit(next, memory_order_relaxed);
    while (current != 0 && current != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                next, &current, current + 1,
                memory_order_relaxed, memory_order_relaxed))
            return current;
    }
    return 0;
}

#endif /* ORES_NATIVE_ACTOR_DOMAIN_ID_H */
