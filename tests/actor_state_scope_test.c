#include "../src/main/cpp/oreslang/runtime/ActorStateScope.h"
#include <assert.h>

int main(void) {
    ores_actor_state_scope a = {.owner_actor_id = 17};
    ores_actor_state_scope b = {.owner_actor_id = 18};
    uint64_t loan = 0, other = 0;
    assert(ores_actor_state_enter(&a, 18, &loan) == ORES_ACTOR_STATE_WRONG_OWNER);
    assert(ores_actor_state_enter(&a, 17, &loan) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_enter(&a, 17, &other) == ORES_ACTOR_STATE_REENTRANT);
    assert(ores_actor_state_check(&a, 18, loan) == ORES_ACTOR_STATE_WRONG_OWNER);
    assert(ores_actor_state_check(&a, 17, loan) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_leave(&a, 17, loan) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_check(&a, 17, loan) == ORES_ACTOR_STATE_STALE);
    assert(ores_actor_state_enter(&a, 17, &other) == ORES_ACTOR_STATE_OK);
    assert(other != loan);
    assert(ores_actor_state_leave(&a, 17, other) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_enter(&b, 18, &other) == ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_leave(&b, 18, other) == ORES_ACTOR_STATE_OK);
    return 0;
}
