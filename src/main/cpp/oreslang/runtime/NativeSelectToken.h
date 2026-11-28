#pragma once
#include <stdint.h>

typedef struct OresNativeSelectArbiter OresNativeSelectArbiter;

typedef struct {
    OresNativeSelectArbiter* arbiter;
    uint64_t selection_generation;
    uint64_t arm_generation;
    uint64_t arm;
} OresNativeSelectToken;
