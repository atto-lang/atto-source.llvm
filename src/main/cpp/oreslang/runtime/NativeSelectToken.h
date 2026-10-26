#pragma once
#include <stdint.h>

typedef struct OresNativeSelectArbiter OresNativeSelectArbiter;

typedef struct {
    OresNativeSelectArbiter* arbiter;
    uint64_t selection_generation;
    uint64_t arm_generation;
    uint64_t arm;

    /*
     * Optional common select-continuation wake identity. These are numeric
     * capability coordinates only so this header remains independent of the
     * scheduler's C ABI types. The scheduler validates every field against its
     * own preallocated WakeCredit before publication.
     */
    uint64_t notify_scheduler_domain;
    uint64_t notify_producer_slot;
    uint64_t notify_producer_generation;
    uint64_t notify_wake_slot;
    uint64_t notify_wake_generation;
} OresNativeSelectToken;
