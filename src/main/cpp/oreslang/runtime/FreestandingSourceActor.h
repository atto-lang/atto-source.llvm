#ifndef ORESLANG_FREESTANDING_SOURCE_ACTOR_H
#define ORESLANG_FREESTANDING_SOURCE_ACTOR_H
#include "oreslang/runtime/FreestandingActorKernel.h"
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Trusted bootstrap/test-only snapshots. Never expose heap/domain/epoch or
 * raw pointer-valued refs in Oreslang guest language. */
typedef struct {
    uint32_t spawned;
    uint32_t retired;
    uint32_t failed;
    uint64_t deliveries;
    uint64_t last_result[ORES_KERNEL_ACTORS];
} OresFreestandingSourceStats;
void ores_freestanding_source_reset(void);
OresFreestandingSourceStats ores_freestanding_source_stats(void);
#ifdef __cplusplus
}
#endif
#endif
