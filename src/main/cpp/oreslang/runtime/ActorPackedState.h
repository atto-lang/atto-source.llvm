#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compiler-only persistent source-actor state.
 *
 * All source actor scalar fields are packed into one ActorArena persistent
 * record. The opaque pointer and turn credentials are scheduler capabilities,
 * never guest-language values. Turn authority remains carrier-bound through
 * ActorArena; migration requires leave + fresh enter.
 */
typedef struct OresActorPackedState OresActorPackedState;
typedef struct { uint64_t domain, epoch; } OresActorPackedStateTurn;

OresActorPackedState* ores_actor_packed_state_open(uint64_t actor_id,
                                                    uint64_t incarnation,
                                                    uint64_t field_count);
OresActorPackedStateTurn ores_actor_packed_state_enter(
    OresActorPackedState*, uint64_t actor_id, uint64_t incarnation);
void ores_actor_packed_state_leave(OresActorPackedState*, OresActorPackedStateTurn);

void ores_actor_packed_state_field_init(OresActorPackedState*,
                                        uint64_t domain, uint64_t epoch,
                                        uint64_t index, int64_t initial);
void ores_actor_packed_state_field_init_mutable(OresActorPackedState*,
                                                uint64_t domain, uint64_t epoch,
                                                uint64_t index, int64_t initial);
int64_t ores_actor_packed_state_field_read(OresActorPackedState*,
                                           uint64_t domain, uint64_t epoch,
                                           uint64_t index);
void ores_actor_packed_state_field_write(OresActorPackedState*,
                                         uint64_t domain, uint64_t epoch,
                                         uint64_t index, int64_t value);

/* Trusted runtime-only scalar publication patch, no guest-visible pointer. */
typedef struct {
    uint64_t index;
    int64_t value;
} OresActorScalarPatch;

/*
 * Version-checked ALL-OR-NOTHING scalar record publication under the
 * existing carrier-bound exclusive actor turn. On stale expected version
 * returns 0 without mutation; on success returns 1. Forged turn, malformed
 * patch, overflow, immutable/uninitialized field and duplicate indices
 * fail-stop. No allocation, guest callback or throw after staging.
 *
 * This is NOT durable publication and NOT an authenticated gradient
 * contribution API. It is a compiler/runtime integration primitive.
 */
int ores_actor_packed_state_compare_publish(OresActorPackedState*,
    uint64_t domain, uint64_t epoch,
    uint64_t version_index, int64_t expected_version,
    const OresActorScalarPatch* patches, uint64_t patch_count);

void ores_actor_packed_state_retire(OresActorPackedState*);
/* Returns 0 while a turn is active; on success zeroizes/frees the arena and
 * the adapter metadata. The supervisor must fence stale callbacks first. */
int ores_actor_packed_state_try_destroy(OresActorPackedState*);

#ifdef __cplusplus
}
#endif
