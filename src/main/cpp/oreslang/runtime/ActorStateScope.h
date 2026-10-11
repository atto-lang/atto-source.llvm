#ifndef ORESLANG_ACTOR_STATE_SCOPE_H
#define ORESLANG_ACTOR_STATE_SCOPE_H
/* Synchronous, scheduler-admitted actor-state guard. Not a cross-await lock.
 * Only the currently admitted logical continuation may enter. A distinct
 * continuation must wait for actor lane admission; actor ID alone is insufficient.
 */
#include <stdbool.h>
#include <stdint.h>
typedef struct ores_actor_state_scope {
    uint64_t owner_actor_id;
    uint64_t generation;
    uint64_t owner_continuation_id;
    uint32_t depth;
    bool active;
} ores_actor_state_scope;
typedef enum ores_actor_state_result {
    ORES_ACTOR_STATE_OK = 0,
    ORES_ACTOR_STATE_WRONG_OWNER = 1,
    ORES_ACTOR_STATE_REENTRANT = 2, /* busy: another logical continuation */
    ORES_ACTOR_STATE_STALE = 3,
    ORES_ACTOR_STATE_OUT_OF_ORDER = 4
} ores_actor_state_result;
static inline ores_actor_state_result
ores_actor_state_enter_continuation(ores_actor_state_scope *scope,
        uint64_t actor_id, uint64_t continuation_id, uint64_t *loan_generation) {
    if (!scope || !loan_generation || !actor_id || !continuation_id ||
        scope->owner_actor_id != actor_id) return ORES_ACTOR_STATE_WRONG_OWNER;
    if (scope->active) {
        if (scope->owner_continuation_id != continuation_id)
            return ORES_ACTOR_STATE_REENTRANT;
        if (scope->depth == UINT32_MAX) return ORES_ACTOR_STATE_OUT_OF_ORDER;
        ++scope->depth;
        *loan_generation = scope->generation;
        return ORES_ACTOR_STATE_OK;
    }
    ++scope->generation;
    if (!scope->generation) ++scope->generation;
    scope->active = true;
    scope->owner_continuation_id = continuation_id;
    scope->depth = 1;
    *loan_generation = scope->generation;
    return ORES_ACTOR_STATE_OK;
}
static inline ores_actor_state_result
ores_actor_state_check_continuation(const ores_actor_state_scope *scope,
        uint64_t actor_id, uint64_t continuation_id, uint64_t generation) {
    if (!scope || !scope->active || scope->generation != generation)
        return ORES_ACTOR_STATE_STALE;
    if (scope->owner_actor_id != actor_id || !continuation_id ||
        scope->owner_continuation_id != continuation_id)
        return ORES_ACTOR_STATE_WRONG_OWNER;
    return ORES_ACTOR_STATE_OK;
}
static inline ores_actor_state_result
ores_actor_state_leave_continuation(ores_actor_state_scope *scope,
        uint64_t actor_id, uint64_t continuation_id, uint64_t generation) {
    ores_actor_state_result result = ores_actor_state_check_continuation(
        scope, actor_id, continuation_id, generation);
    if (result != ORES_ACTOR_STATE_OK) return result;
    if (--scope->depth == 0) {
        scope->active = false;
        scope->owner_continuation_id = 0;
    }
    return ORES_ACTOR_STATE_OK;
}
/* Legacy API: actor ID serves as the continuation ID only for synchronous,
 * single-flight host callers. The scheduler must use the explicit token API. */
static inline ores_actor_state_result
ores_actor_state_enter(ores_actor_state_scope *scope, uint64_t actor_id,
                       uint64_t *generation) {
    return ores_actor_state_enter_continuation(scope, actor_id, actor_id, generation);
}
static inline ores_actor_state_result
ores_actor_state_check(const ores_actor_state_scope *scope, uint64_t actor_id,
                       uint64_t generation) {
    return ores_actor_state_check_continuation(scope, actor_id, actor_id, generation);
}
static inline ores_actor_state_result
ores_actor_state_leave(ores_actor_state_scope *scope, uint64_t actor_id,
                       uint64_t generation) {
    return ores_actor_state_leave_continuation(scope, actor_id, actor_id, generation);
}
#endif
