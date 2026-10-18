#ifndef ORESLANG_FREESTANDING_SOURCE_ACTOR_H
#define ORESLANG_FREESTANDING_SOURCE_ACTOR_H
#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/FreestandingActorKernel.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Trusted bootstrap/test-only snapshots. No actor-private pointers escape. */
typedef struct {
    uint32_t spawned;
    uint32_t retired;
    uint64_t delivered;
    int64_t last_result[ORES_KERNEL_ACTORS];
} OresFreestandingActorStats;
void ores_freestanding_source_stats(OresFreestandingActorStats *out);
#ifdef __cplusplus
}
#endif
#endif
