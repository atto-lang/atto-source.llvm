#include "oreslang/runtime/ActorManagedRefState.h"

#include <stdlib.h>
#include <string.h>

struct OresActorManagedRefState {
    uint64_t domain;
    size_t field_count;
    uint32_t slots[ORES_ACTOR_MANAGED_REF_MAX_FIELDS];
};

static void deny(void) {
    abort();
}

OresActorManagedRefState* ores_actor_managed_ref_state_open(
        uint64_t domain, size_t field_count) {
    if (!domain || !field_count ||
            field_count > ORES_ACTOR_MANAGED_REF_MAX_FIELDS)
        deny();
    OresActorManagedRefState* state =
        (OresActorManagedRefState*)calloc(1, sizeof(*state));
    if (!state) deny();
    state->domain = domain;
    state->field_count = field_count;
    return state;
}

void ores_actor_managed_ref_state_init_i32(
        OresActorManagedRefState* state,
        uint64_t domain, size_t index, uint32_t bits) {
    if (!state || state->domain != domain ||
            index >= state->field_count || !bits ||
            state->slots[index] != 0)
        deny();
    state->slots[index] = bits;
}

uint32_t ores_actor_managed_ref_state_load_i32(
        const OresActorManagedRefState* state,
        uint64_t domain, size_t index) {
    if (!state || state->domain != domain ||
            index >= state->field_count || !state->slots[index])
        deny();
    return state->slots[index];
}

void ores_actor_managed_ref_state_destroy(OresActorManagedRefState* state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    free(state);
}
