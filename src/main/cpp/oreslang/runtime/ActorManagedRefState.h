#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORES_ACTOR_MANAGED_REF_MAX_FIELDS 16u

typedef struct OresActorManagedRefState OresActorManagedRefState;

/*
 * Compiler/runtime-owned physical storage for actor-local managed references.
 * Slots are exactly uint32_t; the opaque state object additionally binds them
 * to one ActorArena domain so a host cannot pair another actor's ref column
 * with a valid compact-reference bit pattern.
 */
OresActorManagedRefState* ores_actor_managed_ref_state_open(
    uint64_t domain, size_t field_count);
void ores_actor_managed_ref_state_init_i32(
    OresActorManagedRefState*, uint64_t domain, size_t index, uint32_t bits);
uint32_t ores_actor_managed_ref_state_load_i32(
    const OresActorManagedRefState*, uint64_t domain, size_t index);
void ores_actor_managed_ref_state_destroy(OresActorManagedRefState*);

#ifdef __cplusplus
}
#endif
