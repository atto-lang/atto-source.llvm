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
    require_index(state, index);
    if (state->initialized[index]) deny();
    OresArenaTurn turn = {domain, epoch};
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
    require_index(state, index);
    if (!state->initialized[index]) deny();
    int64_t value = 0;
    OresArenaTurn turn = {domain, epoch};
    ores_actor_arena_read(
        state->arena, turn, state->record,
        (size_t) index * sizeof(int64_t), &value, sizeof(value));
    return value;
}

void ores_actor_packed_state_field_write(OresActorPackedState* state,
                                         uint64_t domain, uint64_t epoch,
                                         uint64_t index, int64_t value) {
    require_index(state, index);
    if (!state->initialized[index] || !state->mutable_fields[index]) deny();
    OresArenaTurn turn = {domain, epoch};
    ores_actor_arena_write(
        state->arena, turn, state->record,
        (size_t) index * sizeof(int64_t), &value, sizeof(value));
}

int ores_actor_packed_state_compare_publish(OresActorPackedState* state,
    uint64_t domain, uint64_t epoch,
    uint64_t version_index, int64_t expected_version,
    const OresActorScalarPatch* patches, uint64_t patch_count) {
    require_index(state, version_index);
    if (!patches || !patch_count || patch_count > state->fields
        || expected_version < 0 || expected_version == INT64_MAX
        || !state->initialized[version_index]
        || !state->mutable_fields[version_index])
        deny();
    const size_t record_bytes = (size_t)state->fields * sizeof(int64_t);
    OresArenaTurn turn = {domain, epoch};
    /* ActorArena authenticates the admitted physical carrier and turn.
     * The single-record read validates bounds/loan lifetime before staging. */
    int64_t staging[ORES_PACKED_STATE_MAX_FIELDS];
    ores_actor_arena_read(state->arena, turn, state->record,
                          0, staging, record_bytes);
    if (staging[version_index] != expected_version)
        return 0;  /* CAS miss is recoverable and has no side effects. */

    unsigned char seen[ORES_PACKED_STATE_MAX_FIELDS] = {0};
    for (uint64_t i = 0; i < patch_count; ++i) {
        const uint64_t index = patches[i].index;
        if (index >= state->fields || seen[index]
            || !state->initialized[index] || !state->mutable_fields[index])
            deny();
        seen[index] = 1;
        staging[index] = patches[i].value;
    }
    /* Version must advance once. No plain write or incomplete optimizer
     * image may reuse the old version under this API. */
    if (!seen[version_index]
        || staging[version_index] != expected_version + INT64_C(1))
        deny();

    /* Entire state image is copied under one ActorArena write lock.
     * All allocations, graph work and tensor kernels are OUTSIDE this
     * call; the final write has no intermediate guest-visible state. */
    ores_actor_arena_write(state->arena, turn, state->record,
                           0, staging, record_bytes);
    return 1;
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
