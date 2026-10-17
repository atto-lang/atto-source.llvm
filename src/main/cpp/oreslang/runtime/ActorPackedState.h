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

void ores_actor_packed_state_retire(OresActorPackedState*);
/* Returns 0 while a turn is active; on success zeroizes/frees the arena and
 * the adapter metadata. The supervisor must fence stale callbacks first. */
int ores_actor_packed_state_try_destroy(OresActorPackedState*);

#ifdef __cplusplus
}
#endif
