#ifndef ORESLANG_ACTOR_STATE_RW_ADMISSION_H
#define ORESLANG_ACTOR_STATE_RW_ADMISSION_H
/*
 * Oreslang actor-state synchronization bridge (native runtime only).
 * A concrete actor has exactly one persistent state aggregate. This gate
 * protects its compiled payload but does not expose any raw state pointer.
 * The compiler MUST lower sealed handle_state using an authenticated
 * ActorRuntime admission. Guest code must not call this C API or forge IDs.
 * No OS wait, heap allocation, Java monitor or thread-affine owner.
 */
#include "oreslang/runtime/OresStdSyncAdmission.h"
#include <stdint.h>

#define ORES_STATE_EFFECT_READ    0x01u
#define ORES_STATE_EFFECT_WRITE   0x02u
#define ORES_STATE_EFFECT_SUSPEND 0x04u
#define ORES_STATE_EFFECT_IO      0x08u
#define ORES_STATE_EFFECT_FFI     0x10u
#define ORES_STATE_EFFECT_MASK    0x1fu

typedef enum ores_state_admission_code {
    ORES_STATE_ALLOWED = 0,
    ORES_STATE_WRONG_ACTOR = 1,
    ORES_STATE_EFFECT_DENIED = 2,
    ORES_STATE_NOT_CRITICAL = 3,
    ORES_STATE_INVALID = 4
} ores_state_admission_code;

typedef struct ores_actor_state_rw {
    uint64_t actor_id;
    uint64_t incarnation;
    ores_std_sync_gate gate;
} ores_actor_state_rw;

#define ORES_ACTOR_STATE_RW_INIT(ACTOR_ID, INCARNATION) { \
    .actor_id=(ACTOR_ID), .incarnation=(INCARNATION), .gate=ORES_STD_RW_INIT_SCOPE(INCARNATION) }

typedef struct ores_actor_state_acquisition {
    ores_state_admission_code verification;
    ores_std_sync_result admission;
} ores_actor_state_acquisition;

static inline bool ores_state_identity_valid(
        const ores_actor_state_rw* state, uint64_t actor_id,
        uint64_t incarnation) {
    return state && actor_id && incarnation &&
           state->actor_id == actor_id && state->incarnation == incarnation;
}
static inline ores_actor_state_acquisition ores_state_handle_request(
        ores_actor_state_rw* state, uint64_t trusted_actor_id,
        uint64_t trusted_incarnation, bool compiler_verified_critical,
        unsigned int declared_using, unsigned int inferred_effects,
        ores_std_access mode) {
    ores_actor_state_acquisition out = {0};
    if (!state || (mode != ORES_STD_READ && mode != ORES_STD_WRITE)) {
        out.verification = ORES_STATE_INVALID;
    } else if (!ores_state_identity_valid(state, trusted_actor_id, trusted_incarnation)) {
        out.verification = ORES_STATE_WRONG_ACTOR;
    } else if (!compiler_verified_critical) {
        out.verification = ORES_STATE_NOT_CRITICAL;
    } else if ((declared_using | inferred_effects) & ~ORES_STATE_EFFECT_MASK ||
               (inferred_effects & ~declared_using) ||
               /* A read loan must never admit a callback that mutates state,
                * even if the caller declared both read and write effects. */
               (mode == ORES_STD_READ && (inferred_effects & ORES_STATE_EFFECT_WRITE)) ||
               /* This synchronous bridge cannot pin a borrowed state view
                * across suspend or expose it to arbitrary FFI. */
               (inferred_effects & (ORES_STATE_EFFECT_SUSPEND | ORES_STATE_EFFECT_FFI)) ||
               !(declared_using & (mode == ORES_STD_READ
                   ? ORES_STATE_EFFECT_READ : ORES_STATE_EFFECT_WRITE)) ||
               !(inferred_effects & (mode == ORES_STD_READ
                   ? ORES_STATE_EFFECT_READ : ORES_STATE_EFFECT_WRITE))) {
        out.verification = ORES_STATE_EFFECT_DENIED;
    } else {
        out.verification = ORES_STATE_ALLOWED;
        out.admission = ores_std_request(&state->gate, mode);
    }
    return out;
}
/* A synchronous compiled callback must start exactly once, finish even on
 * unwind, and release its ticket. Direct nested state loans need distinct
 * verified tickets and must obey read/write compatibility. */
static inline ores_std_sync_code ores_state_start(
        ores_actor_state_rw* state, uint64_t trusted_actor_id,
        uint64_t trusted_incarnation, ores_std_ticket ticket) {
    if (!ores_state_identity_valid(state, trusted_actor_id, trusted_incarnation))
        return ORES_STD_SYNC_INVALID;
    return ores_std_start(&state->gate, ticket);
}
static inline ores_std_sync_code ores_state_end(
        ores_actor_state_rw* state, uint64_t trusted_actor_id,
        uint64_t trusted_incarnation, ores_std_ticket ticket) {
    if (!ores_state_identity_valid(state, trusted_actor_id, trusted_incarnation))
        return ORES_STD_SYNC_INVALID;
    return ores_std_end(&state->gate, ticket);
}
static inline ores_std_sync_result ores_state_release(
        ores_actor_state_rw* state, uint64_t trusted_actor_id,
        uint64_t trusted_incarnation, ores_std_ticket ticket) {
    if (!ores_state_identity_valid(state, trusted_actor_id, trusted_incarnation))
        return ores_std_empty(ORES_STD_SYNC_INVALID);
    return ores_std_release(&state->gate, ticket);
}
static inline ores_std_sync_code ores_state_ready_to_suspend(
        ores_actor_state_rw* state, uint64_t trusted_actor_id,
        uint64_t trusted_incarnation, ores_std_ticket ticket) {
    if (!ores_state_identity_valid(state, trusted_actor_id, trusted_incarnation))
        return ORES_STD_SYNC_INVALID;
    return ores_std_ready_to_suspend(&state->gate, ticket);
}
static inline ores_std_sync_result ores_state_stop(
        ores_actor_state_rw* state, uint64_t trusted_actor_id,
        uint64_t trusted_incarnation) {
    if (!ores_state_identity_valid(state, trusted_actor_id, trusted_incarnation))
        return ores_std_empty(ORES_STD_SYNC_INVALID);
    return ores_std_seal(&state->gate);
}
static inline ores_std_sync_code ores_state_quiescent(
        ores_actor_state_rw* state, uint64_t trusted_actor_id,
        uint64_t trusted_incarnation) {
    if (!ores_state_identity_valid(state, trusted_actor_id, trusted_incarnation))
        return ORES_STD_SYNC_INVALID;
    return ores_std_quiescent(&state->gate);
}
#endif
