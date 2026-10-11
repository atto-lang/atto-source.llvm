#include "../src/main/cpp/oreslang/runtime/ActorStateScope.h"
#include <assert.h>
int main(void) {
    ores_actor_state_scope a = {.owner_actor_id = 17};
    ores_actor_state_scope b = {.owner_actor_id = 18};
    uint64_t outer = 0, inner = 0, other = 0;
    assert(ores_actor_state_enter_continuation(&a,18,101,&outer)==ORES_ACTOR_STATE_WRONG_OWNER);
    assert(ores_actor_state_enter_continuation(&a,17,101,&outer)==ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_enter_continuation(&a,17,101,&inner)==ORES_ACTOR_STATE_OK);
    assert(inner==outer && a.depth==2);
    assert(ores_actor_state_enter_continuation(&a,17,202,&other)==ORES_ACTOR_STATE_REENTRANT);
    assert(ores_actor_state_check_continuation(&a,17,202,outer)==ORES_ACTOR_STATE_WRONG_OWNER);
    assert(ores_actor_state_leave_continuation(&a,17,101,inner)==ORES_ACTOR_STATE_OK);
    assert(a.active && a.depth==1);
    assert(ores_actor_state_check_continuation(&a,17,101,outer)==ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_leave_continuation(&a,17,101,outer)==ORES_ACTOR_STATE_OK);
    assert(!a.active);
    assert(ores_actor_state_check_continuation(&a,17,101,outer)==ORES_ACTOR_STATE_STALE);
    assert(ores_actor_state_enter_continuation(&a,17,202,&other)==ORES_ACTOR_STATE_OK);
    assert(other!=outer);
    assert(ores_actor_state_leave_continuation(&a,17,202,other)==ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_enter(&b,18,&other)==ORES_ACTOR_STATE_OK);
    assert(ores_actor_state_leave(&b,18,other)==ORES_ACTOR_STATE_OK);
    return 0;
}
