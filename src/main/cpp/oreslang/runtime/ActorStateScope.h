#ifndef ORESLANG_ACTOR_STATE_SCOPE_H
#define ORESLANG_ACTOR_STATE_SCOPE_H

/* Synchronous state-access loan fence; compiler/scheduler owns admission.
 * Not a lock and not valid across await. No host mutex or carrier identity.
 * Trusted scheduler-issued actor/continuation IDs are required: caller-supplied
 * identities are NOT an unforgeable capability or a substitute for lane checks.
 * All operations require exclusive logical-lane admission; not thread-safe.
 */
#include <stdbool.h>
#include <stdint.h>

#define ORES_ACTOR_STATE_MAX_NESTING 32u

typedef struct ores_actor_state_scope {
    uint64_t owner_actor_id;
    uint64_t generation;          /* Monotonic loan serial, never reused. */
    uint64_t owner_continuation_id;
    uint64_t loan_stack[ORES_ACTOR_STATE_MAX_NESTING];
    uint32_t depth;
    bool active;
} ores_actor_state_scope;

typedef enum ores_actor_state_result {
    ORES_ACTOR_STATE_OK = 0,
    ORES_ACTOR_STATE_WRONG_OWNER = 1,
    ORES_ACTOR_STATE_REENTRANT = 2, /* different logical continuation */
    ORES_ACTOR_STATE_STALE = 3,
    ORES_ACTOR_STATE_OUT_OF_ORDER = 4,
    ORES_ACTOR_STATE_LIMIT = 5,
    ORES_ACTOR_STATE_BUSY_LOAN = 6
} ores_actor_state_result;

static inline ores_actor_state_result
ores_actor_state_enter_continuation(ores_actor_state_scope *scope,
        uint64_t actor_id, uint64_t continuation_id, uint64_t *loan_token) {
    if (!scope || !loan_token || !actor_id || !continuation_id ||
        scope->owner_actor_id != actor_id)
        return ORES_ACTOR_STATE_WRONG_OWNER;
    if (scope->active && scope->owner_continuation_id != continuation_id)
        return ORES_ACTOR_STATE_REENTRANT;
    if (scope->depth >= ORES_ACTOR_STATE_MAX_NESTING ||
        scope->generation == UINT64_MAX)
        return ORES_ACTOR_STATE_LIMIT;
    const uint64_t next = ++scope->generation;
    if (!scope->active) {
        scope->owner_continuation_id = continuation_id;
        scope->active = true;
    }
    scope->loan_stack[scope->depth++] = next;
    *loan_token = next;
    return ORES_ACTOR_STATE_OK;
}

static inline ores_actor_state_result
ores_actor_state_check_continuation(const ores_actor_state_scope *scope,
        uint64_t actor_id, uint64_t continuation_id, uint64_t loan_token) {
    if (!scope || !scope->active || !scope->depth || !loan_token)
        return ORES_ACTOR_STATE_STALE;
    if (scope->owner_actor_id != actor_id || !continuation_id ||
        scope->owner_continuation_id != continuation_id)
        return ORES_ACTOR_STATE_WRONG_OWNER;
    if (scope->loan_stack[scope->depth - 1] == loan_token)
        return ORES_ACTOR_STATE_OK;
    for (uint32_t i = 0; i + 1 < scope->depth; ++i)
        if (scope->loan_stack[i] == loan_token)
            return ORES_ACTOR_STATE_OUT_OF_ORDER;
    return ORES_ACTOR_STATE_STALE;
}

static inline ores_actor_state_result
ores_actor_state_leave_continuation(ores_actor_state_scope *scope,
        uint64_t actor_id, uint64_t continuation_id, uint64_t loan_token) {
    ores_actor_state_result result = ores_actor_state_check_continuation(
        scope, actor_id, continuation_id, loan_token);
    if (result != ORES_ACTOR_STATE_OK) return result;
    scope->loan_stack[--scope->depth] = 0; /* revoke innermost loan */
    if (scope->depth == 0) {
        scope->active = false;
        scope->owner_continuation_id = 0;
    }
    return ORES_ACTOR_STATE_OK;
}

/*
 * A yielding actor may release its scheduler lane ONLY when there are
 * zero live state-backed access loans. Every loan token has already been
 * revoked by leave_continuation. The scheduler must separately prove that
 * no other actor/FFI resource loan spans the suspension and that re-entry
 * uses a valid actor-incarnation/continuation ticket.
 *
 * This is a gate for a trusted scheduler, not permission for arbitrary guest
 * calls to release actor admission or install forged identities.
 */
static inline ores_actor_state_result
ores_actor_state_ready_to_suspend(const ores_actor_state_scope *scope,
        uint64_t actor_id) {
    if (!scope || !actor_id || scope->owner_actor_id != actor_id)
        return ORES_ACTOR_STATE_WRONG_OWNER;
    if (scope->active || scope->depth)
        return ORES_ACTOR_STATE_BUSY_LOAN;
    return ORES_ACTOR_STATE_OK;
}

/* Legacy synchronous ABI: only safe if scheduler already proved the actor
 * itself is the unique current continuation. Do not use in async lowering. */
static inline ores_actor_state_result
ores_actor_state_enter(ores_actor_state_scope *scope, uint64_t actor_id,
                       uint64_t *loan_token) {
    return ores_actor_state_enter_continuation(scope, actor_id, actor_id, loan_token);
}
static inline ores_actor_state_result
ores_actor_state_check(const ores_actor_state_scope *scope, uint64_t actor_id,
                       uint64_t loan_token) {
    return ores_actor_state_check_continuation(scope, actor_id, actor_id, loan_token);
}
static inline ores_actor_state_result
ores_actor_state_leave(ores_actor_state_scope *scope, uint64_t actor_id,
                       uint64_t loan_token) {
    return ores_actor_state_leave_continuation(scope, actor_id, actor_id, loan_token);
}
#endif
