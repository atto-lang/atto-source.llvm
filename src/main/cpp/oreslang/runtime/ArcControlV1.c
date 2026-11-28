#include "oreslang/runtime/ArcControlV1.h"

static bool valid(const OresArcControlV1* c, uint64_t domain,
                  uint64_t generation) {
    return c && domain != 0 && generation != 0 &&
           atomic_load_explicit(&c->initialized, memory_order_acquire) == 1 &&
           c->domain == domain && c->generation == generation;
}

bool ores_arc_v1_init(OresArcControlV1* c, uint64_t domain,
                      uint64_t generation, void* payload, void* context,
                      OresArcDestroyV1 destroy, OresArcReclaimV1 reclaim) {
    /* Trusted owner must supply fresh ZERO_INIT and coordinate initialization;
     * never concurrently initialize one control block or recycle a live one. */
    if (!c || !domain || !generation || !payload || !destroy || !reclaim ||
        atomic_load_explicit(&c->initialized, memory_order_relaxed) != 0)
        return false;
    c->domain = domain;
    c->generation = generation;
    c->payload = payload;
    c->context = context;
    c->destroy = destroy;
    c->reclaim = reclaim;
    atomic_init(&c->strong, 1);
    atomic_init(&c->weak, 1);
    atomic_init(&c->drop_complete, 0);
    atomic_store_explicit(&c->initialized, 1, memory_order_release);
    return true;
}

static bool increment_if_nonzero(_Atomic uint32_t* counter,
                                 memory_order success_order) {
    uint32_t seen = atomic_load_explicit(counter, memory_order_relaxed);
    while (seen != 0 && seen < ORES_ARC_V1_MAX_OWNERS) {
        if (atomic_compare_exchange_weak_explicit(
                counter, &seen, seen + 1, success_order, memory_order_relaxed))
            return true;
    }
    return false;
}

bool ores_arc_v1_clone_strong(OresArcControlV1* c,
                              uint64_t domain, uint64_t generation) {
    return valid(c, domain, generation) &&
           increment_if_nonzero(&c->strong, memory_order_acquire);
}

bool ores_arc_v1_downgrade(OresArcControlV1* c,
                            uint64_t domain, uint64_t generation) {
    /* The caller is required to possess a live strong owner; a separately
     * authorized strong handle keeps the implicit weak sentinel installed. */
    return valid(c, domain, generation) &&
           atomic_load_explicit(&c->strong, memory_order_acquire) != 0 &&
           increment_if_nonzero(&c->weak, memory_order_relaxed);
}

bool ores_arc_v1_clone_weak(OresArcControlV1* c,
                             uint64_t domain, uint64_t generation) {
    if (!valid(c, domain, generation)) return false;
    /* A caller cannot obtain ownership by cloning the implicit sentinel.
     * The trusted compiler must additionally prove a real weak source. */
    if (atomic_load_explicit(&c->drop_complete, memory_order_acquire) == 0 &&
        atomic_load_explicit(&c->weak, memory_order_relaxed) == 1)
        return false;
    return increment_if_nonzero(&c->weak, memory_order_relaxed);
}

bool ores_arc_v1_upgrade(OresArcControlV1* c,
                          uint64_t domain, uint64_t generation) {
    /* Caller must own an external weak count; CAS with acquire ordering
     * linearizes upgrade against last-strong release. Never 0 -> 1. */
    return valid(c, domain, generation) &&
           increment_if_nonzero(&c->strong, memory_order_acquire);
}

static bool decrement_if_nonzero(_Atomic uint32_t* counter,
                                 uint32_t* before) {
    uint32_t seen = atomic_load_explicit(counter, memory_order_relaxed);
    while (seen != 0) {
        if (atomic_compare_exchange_weak_explicit(
                counter, &seen, seen - 1, memory_order_acq_rel,
                memory_order_relaxed)) {
            *before = seen;
            return true;
        }
    }
    return false;
}

static void release_weak_count(OresArcControlV1* c) {
    uint32_t before = 0;
    /* Called with actual weak owner or privileged implicit weak sentinel. */
    if (!decrement_if_nonzero(&c->weak, &before)) return;
    if (before == 1) {
        atomic_thread_fence(memory_order_acquire);
        /* Real payload drop must precede reclaim. This is the only path
         * where an owning allocator may recycle/deallocate the control. */
        if (atomic_load_explicit(&c->drop_complete, memory_order_acquire) == 1)
            c->reclaim(c->context, c);
    }
}

bool ores_arc_v1_release_strong(OresArcControlV1* c,
                                uint64_t domain, uint64_t generation) {
    if (!valid(c, domain, generation)) return false;
    uint32_t before = 0;
    if (!decrement_if_nonzero(&c->strong, &before)) return false;
    if (before == 1) {
        /* Exactly one thread can observe the 1->0 transition; no successful
         * new upgrades can follow. Outstanding weak owners hold control live. */
        atomic_thread_fence(memory_order_acquire);
        c->destroy(c->context, c->payload);
        c->payload = 0;
        atomic_store_explicit(&c->drop_complete, 1, memory_order_release);
        release_weak_count(c); /* implicit sentinel retired after destruction */
        /* c may be reclaimed here: never touch it again. */
    }
    return true;
}

bool ores_arc_v1_release_weak(OresArcControlV1* c,
                              uint64_t domain, uint64_t generation) {
    if (!valid(c, domain, generation)) return false;
    /* Weak external ownership is compiler-controlled. The sentinel must never
     * be released through this guest-generated operation. */
    if (atomic_load_explicit(&c->drop_complete, memory_order_acquire) == 0 &&
        atomic_load_explicit(&c->weak, memory_order_relaxed) == 1)
        return false;
    release_weak_count(c);
    return true;
}
