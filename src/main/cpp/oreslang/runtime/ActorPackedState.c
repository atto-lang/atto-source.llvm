#include "oreslang/runtime/ActorPackedState.h"
#include "oreslang/runtime/ActorArena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ORES_PACKED_STATE_MAX_FIELDS 4096u
#define ORES_PACKED_STATE_SCRATCH_BYTES 64u

struct OresActorPackedState {
    OresActorArena* arena;
    OresArenaHandle record;
    uint64_t owner;
    uint64_t incarnation;
    uint64_t fields;
    unsigned char* initialized;
    unsigned char* mutable_fields;
    int record_ready;
};

static void deny(void) { abort(); }

static OresArenaTurn arena_turn(OresActorPackedStateTurn turn) {
    OresArenaTurn converted = {turn.domain, turn.epoch};
    return converted;
}

static void require_index(OresActorPackedState* state, uint64_t index) {
    if (!state || !state->arena || index >= state->fields || !state->record_ready)
        deny();
}

/*
 * Prove carrier-bound turn authority before reading mutable adapter metadata.
 * A zero-byte read still validates the arena turn and exact opaque record
 * capability, but cannot expose or modify actor memory.
 */
static OresArenaTurn require_field_turn(OresActorPackedState* state,
                                        uint64_t domain, uint64_t epoch) {
    if (!state || !state->arena) deny();
    OresArenaTurn turn = {domain, epoch};
    ores_actor_arena_read(state->arena, turn, state->record, 0, NULL, 0);
    return turn;
}

OresActorPackedState* ores_actor_packed_state_open(uint64_t actor_id,
                                                    uint64_t incarnation,
                                                    uint64_t field_count) {
    if (!actor_id || !incarnation || !field_count
        || field_count > ORES_PACKED_STATE_MAX_FIELDS)
        deny();
    OresActorPackedState* state =
        (OresActorPackedState*) calloc(1, sizeof(*state));
    if (!state) deny();
    state->initialized = (unsigned char*) calloc((size_t) field_count, 1);
    state->mutable_fields = (unsigned char*) calloc((size_t) field_count, 1);
    if (!state->initialized || !state->mutable_fields) deny();
    state->owner = actor_id;
    state->incarnation = incarnation;
    state->fields = field_count;
    state->arena = ores_actor_arena_open(
        actor_id, incarnation, 1,
        (size_t) field_count * sizeof(int64_t),
        ORES_PACKED_STATE_SCRATCH_BYTES);
    if (!state->arena) deny();
    return state;
}

OresActorPackedStateTurn ores_actor_packed_state_enter(
    OresActorPackedState* state, uint64_t actor_id, uint64_t incarnation) {
    if (!state || !state->arena || actor_id != state->owner
        || incarnation != state->incarnation)
        deny();
    OresArenaTurn turn =
        ores_actor_arena_enter(state->arena, actor_id, incarnation);
    if (!state->record_ready) {
        state->record = ores_actor_arena_alloc(
            state->arena, turn, (size_t) state->fields * sizeof(int64_t));
        state->record_ready = 1;
    }
    OresActorPackedStateTurn result = {turn.domain, turn.epoch};
    return result;
}

void ores_actor_packed_state_leave(OresActorPackedState* state,
                                   OresActorPackedStateTurn turn) {
    if (!state || !state->arena) deny();
    ores_actor_arena_leave(state->arena, arena_turn(turn));
}

static void initialize(OresActorPackedState* state,
                       uint64_t domain, uint64_t epoch,
                       uint64_t index, int64_t initial, int is_mutable) {
    OresArenaTurn turn = require_field_turn(state, domain, epoch);
    require_index(state, index);
    if (state->initialized[index]) deny();
    ores_actor_arena_write(
        state->arena, turn, state->record,
        (size_t) index * sizeof(int64_t), &initial, sizeof(initial));
    state->initialized[index] = 1;
    state->mutable_fields[index] = is_mutable ? 1 : 0;
}

void ores_actor_packed_state_field_init(OresActorPackedState* state,
                                        uint64_t domain, uint64_t epoch,
                                        uint64_t index, int64_t initial) {
    initialize(state, domain, epoch, index, initial, 0);
}

void ores_actor_packed_state_field_init_mutable(OresActorPackedState* state,
                                                uint64_t domain, uint64_t epoch,
                                                uint64_t index, int64_t initial) {
    initialize(state, domain, epoch, index, initial, 1);
}

int64_t ores_actor_packed_state_field_read(OresActorPackedState* state,
                                           uint64_t domain, uint64_t epoch,
                                           uint64_t index) {
    OresArenaTurn turn = require_field_turn(state, domain, epoch);
    require_index(state, index);
    if (!state->initialized[index]) deny();
    int64_t value = 0;
    ores_actor_arena_read(
        state->arena, turn, state->record,
        (size_t) index * sizeof(int64_t), &value, sizeof(value));
    return value;
}

void ores_actor_packed_state_field_write(OresActorPackedState* state,
                                         uint64_t domain, uint64_t epoch,
                                         uint64_t index, int64_t value) {
    OresArenaTurn turn = require_field_turn(state, domain, epoch);
    require_index(state, index);
    if (!state->initialized[index] || !state->mutable_fields[index]) deny();
    ores_actor_arena_write(
        state->arena, turn, state->record,
        (size_t) index * sizeof(int64_t), &value, sizeof(value));
}

void ores_actor_packed_state_retire(OresActorPackedState* state) {
    if (!state || !state->arena) deny();
    ores_actor_arena_retire(state->arena);
}

int ores_actor_packed_state_try_destroy(OresActorPackedState* state) {
    if (!state || !state->arena) deny();
    if (!ores_actor_arena_try_destroy(state->arena)) return 0;
    free(state->initialized);
    free(state->mutable_fields);
    memset(state, 0, sizeof(*state));
    free(state);
    return 1;
}
