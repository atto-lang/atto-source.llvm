#ifndef ORESLANG_ACTOR_STATE_SCOPE_H
#define ORESLANG_ACTOR_STATE_SCOPE_H

/* Compiler-facing, non-suspending state callback guard. This is not a
 * replacement for scheduler-owned actor admission or ownership analysis.
 * Each actor instance owns its own state allocation and scope depth.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct ores_actor_state_scope {
    uint64_t owner_actor_id;
    uint64_t generation;
    bool active;
} ores_actor_state_scope;

typedef enum ores_actor_state_result {
    ORES_ACTOR_STATE_OK = 0,
    ORES_ACTOR_STATE_WRONG_OWNER = 1,
    ORES_ACTOR_STATE_REENTRANT = 2,
    ORES_ACTOR_STATE_STALE = 3
} ores_actor_state_result;

static inline ores_actor_state_result
 ores_actor_state_enter(ores_actor_state_scope *scope, uint64_t caller_actor_id,
                        uint64_t *loan_generation) {
    if (!scope || !loan_generation || !caller_actor_id ||
        scope->owner_actor_id != caller_actor_id)
        return ORES_ACTOR_STATE_WRONG_OWNER;
    if (scope->active) return ORES_ACTOR_STATE_REENTRANT;
    scope->generation++;
    if (!scope->generation) scope->generation++;
    scope->active = true;
    *loan_generation = scope->generation;
    return ORES_ACTOR_STATE_OK;
}

static inline ores_actor_state_result
 ores_actor_state_check(const ores_actor_state_scope *scope,
                        uint64_t caller_actor_id, uint64_t loan_generation) {
    if (!scope || !scope->active || scope->generation != loan_generation)
        return ORES_ACTOR_STATE_STALE;
    return scope->owner_actor_id == caller_actor_id ? ORES_ACTOR_STATE_OK
                                                   : ORES_ACTOR_STATE_WRONG_OWNER;
}

static inline ores_actor_state_result
 ores_actor_state_leave(ores_actor_state_scope *scope,
                        uint64_t caller_actor_id, uint64_t loan_generation) {
    ores_actor_state_result result =
        ores_actor_state_check(scope, caller_actor_id, loan_generation);
    if (result == ORES_ACTOR_STATE_OK) scope->active = false;
    return result;
}
#endif
