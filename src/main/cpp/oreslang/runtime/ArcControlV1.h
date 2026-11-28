#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compiler-internal Arc/Weak control block. This is NOT a guest-visible handle.
 * The owner supplies zero-initialized, aligned backing storage and a separately
 * authenticated (domain,generation) proof. C callers must possess a live,
 * linear strong/weak handle before invoking the corresponding operation.
 * Each successful clone/downgrade/upgrade produces precisely ONE new handle,
 * which must eventually be retired precisely once. The compiler must enforce
 * that discipline; these functions cannot validate arbitrary forged pointers.
 * No malloc, free, VM GC, worker, or background destructor is used here.
 * Reuse the storage only AFTER the final reclaimer callback and with a new
 * generation; never expose stale original proofs to guest code.
 */
#define ORES_ARC_V1_MAX_OWNERS UINT32_C(1048576)

typedef struct OresArcControlV1 OresArcControlV1;
typedef void (*OresArcDestroyV1)(void* context, void* payload);
typedef void (*OresArcReclaimV1)(void* context, OresArcControlV1* control);

struct OresArcControlV1 {
    _Atomic uint32_t strong;
    _Atomic uint32_t weak; /* includes implicit weak until destructor completes */
    _Atomic uint32_t drop_complete;
    _Atomic uint32_t initialized;
    uint64_t domain;
    uint64_t generation;
    void* payload;
    void* context;
    OresArcDestroyV1 destroy;
    OresArcReclaimV1 reclaim;
};

#define ORES_ARC_CONTROL_V1_ZERO_INIT {0}

bool ores_arc_v1_init(OresArcControlV1* control, uint64_t domain,
                      uint64_t generation, void* payload, void* context,
                      OresArcDestroyV1 destroy, OresArcReclaimV1 reclaim);

/* Successful calls mint exactly one new counted owner. Caller must never pass
 * an unowned, stale, guessed or externally supplied control-block pointer. */
bool ores_arc_v1_clone_strong(OresArcControlV1* control,
                              uint64_t domain, uint64_t generation);
bool ores_arc_v1_downgrade(OresArcControlV1* control,
                            uint64_t domain, uint64_t generation);
bool ores_arc_v1_clone_weak(OresArcControlV1* control,
                             uint64_t domain, uint64_t generation);
bool ores_arc_v1_upgrade(OresArcControlV1* control,
                          uint64_t domain, uint64_t generation);

/* Release exactly one real owner; these may call destroy/reclaim before they
 * return. Both callbacks MUST be nonthrowing and must not reenter the control
 * block. A callback must never destroy the block before reclaim is invoked. */
bool ores_arc_v1_release_strong(OresArcControlV1* control,
                                uint64_t domain, uint64_t generation);
bool ores_arc_v1_release_weak(OresArcControlV1* control,
                              uint64_t domain, uint64_t generation);

#ifdef __cplusplus
}
#endif
