#include "../src/main/cpp/oreslang/runtime/ActorStateScope.h"
#include <stdlib.h>

/* Executable Release-mode checks: require() would be elided by -DNDEBUG. */
static void require(int condition) {
    if (!condition) abort();
}
#include <stdint.h>

int main(void) {
    ores_actor_state_scope a = {.owner_actor_id = 17};
    ores_actor_state_scope b = {.owner_actor_id = 18};
    uint64_t outer = 0, inner = 0, other = 0;
    require(ores_actor_state_enter_continuation(&a, 18, 101, &outer) == ORES_ACTOR_STATE_WRONG_OWNER);
    require(ores_actor_state_enter_continuation(&a, 17, 0, &outer) == ORES_ACTOR_STATE_WRONG_OWNER);
    require(ores_actor_state_enter_continuation(&a, 17, 101, &outer) == ORES_ACTOR_STATE_OK);
    require(ores_actor_state_enter_continuation(&a, 17, 101, &inner) == ORES_ACTOR_STATE_OK);
    require(inner != outer && a.depth == 2);
    require(ores_actor_state_enter_continuation(&a, 17, 202, &other) == ORES_ACTOR_STATE_REENTRANT);
    require(ores_actor_state_check_continuation(&a, 17, 202, inner) == ORES_ACTOR_STATE_WRONG_OWNER);
    require(ores_actor_state_check_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OUT_OF_ORDER);
    require(ores_actor_state_leave_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OUT_OF_ORDER);
    require(a.depth == 2);
    require(ores_actor_state_check_continuation(&a, 17, 101, inner) == ORES_ACTOR_STATE_OK);
    require(ores_actor_state_leave_continuation(&a, 17, 101, inner) == ORES_ACTOR_STATE_OK);
    require(a.depth == 1 && a.active);
    require(ores_actor_state_check_continuation(&a, 17, 101, inner) == ORES_ACTOR_STATE_STALE);
    require(ores_actor_state_check_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OK);
    require(ores_actor_state_leave_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_OK);
    require(!a.active && a.depth == 0);
    require(ores_actor_state_leave_continuation(&a, 17, 101, outer) == ORES_ACTOR_STATE_STALE);
    require(ores_actor_state_enter_continuation(&a, 17, 202, &other) == ORES_ACTOR_STATE_OK);
    require(other != outer && other != inner);
    require(ores_actor_state_leave_continuation(&a, 17, 202, other) == ORES_ACTOR_STATE_OK);
    require(ores_actor_state_enter(&b, 18, &other) == ORES_ACTOR_STATE_OK);
    require(ores_actor_state_leave(&b, 18, other) == ORES_ACTOR_STATE_OK);

    uint64_t tokens[ORES_ACTOR_STATE_MAX_NESTING];
    for (uint32_t i = 0; i < ORES_ACTOR_STATE_MAX_NESTING; ++i)
        require(ores_actor_state_enter_continuation(&a, 17, 101, &tokens[i]) == ORES_ACTOR_STATE_OK);
    require(ores_actor_state_enter_continuation(&a, 17, 101, &other) == ORES_ACTOR_STATE_LIMIT);
    require(a.depth == ORES_ACTOR_STATE_MAX_NESTING);
    for (uint32_t i = ORES_ACTOR_STATE_MAX_NESTING; i > 0; --i)
        require(ores_actor_state_leave_continuation(&a, 17, 101, tokens[i - 1]) == ORES_ACTOR_STATE_OK);
    require(a.depth == 0 && !a.active);

    a.generation = UINT64_MAX;
    require(ores_actor_state_enter_continuation(&a, 17, 101, &other) == ORES_ACTOR_STATE_LIMIT);
    require(!a.active);
    return 0;
}
