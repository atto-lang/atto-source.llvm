#include "../src/main/cpp/oreslang/runtime/ActorStateScope.h"
#include <assert.h>
#include <stdint.h>

int main(void) {
    ores_actor_state_scope a = {.owner_actor_id = 17};
    ores_actor_state_scope b = {.owner_actor_id = 18};
    uint64_t outer = 0, inner = 0, other = 0;
    assert(ores_actor_state_enter_continuation(&a, 18, 101, &outer) == ORES_ACTOR_STATE_WRONG_OWNER);
    assert(ores_actor_state_enter_continuation(&a, 17, 0, &outer) == ORES_ACTOR_STATE_WRONG_OWNER);
    assert(ores_actor_state_enter_continuation(&a, 17, 101, &outer) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_enter_continuation(&a, 17, 101, &inner) == ORES_ACTOR_STATE_OK);
    assert(inner != outer && a.depth == 2);
    assert(ores_actor_state_enter_continuation(&a, 17, 202, &other) == ORES_ACTOR_STATE_REENTRANT);
    assert(ores_actor_state_check_continuation(&a, 17, 202, inner) == ORES_ACTOR_STATE_WRONG_OWNER);
    assert(ores_actor_state_check_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OUT_OF_ORDER);
    assert(ores_actor_state_leave_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OUT_OF_ORDER);
    assert(a.depth == 2);
    assert(ores_actor_state_check_continuation(&a, 17, 101, inner) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_leave_continuation(&a, 17, 101, inner) == ORES_ACTOR_STATE_OK);
    assert(a.depth == 1 && a.active);
    assert(ores_actor_state_check_continuation(&a, 17, 101, inner) == ORES_ACTOR_STATE_STALE);
    assert(ores_actor_state_check_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_leave_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OK);
    assert(!a.active && a.depth == 0);
    assert(ores_actor_state_leave_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_STALE);
    assert(ores_actor_state_enter_continuation(&a, 17, 202, &other) == ORES_ACTOR_STATE_OK);
    assert(other != outer && other != inner);
    assert(ores_actor_state_leave_continuation(&a, 17, 202, other) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_enter(&b, 18, &other) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_leave(&b, 18, other) == ORES_ACTOR_STATE_OK);

    uint64_t tokens[ORES_ACTOR_STATE_MAX_NESTING];
    for (uint32_t i = 0; i < ORES_ACTOR_STATE_MAX_NESTING; ++i)
        assert(ores_actor_state_enter_continuation(&a, 17, 101, &tokens[i]) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_enter_continuation(&a, 17, 101, &other) == ORES_ACTOR_STATE_LIMIT);
    assert(a.depth == ORES_ACTOR_STATE_MAX_NESTING);
    for (uint32_t i = ORES_ACTOR_STATE_MAX_NESTING; i > 0; --i)
        assert(ores_actor_state_leave_continuation(&a, 17, 101, tokens[i - 1]) == ORES_ACTOR_STATE_OK);
    assert(a.depth == 0 && !a.active);

    a.generation = UINT64_MAX;
    assert(ores_actor_state_enter_continuation(&a, 17, 101, &other) == ORES_ACTOR_STATE_LIMIT);
    assert(!a.active);
    return 0;
}
