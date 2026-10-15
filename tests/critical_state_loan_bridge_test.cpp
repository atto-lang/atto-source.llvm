#include "oreslang/runtime/CriticalStateLoan.hpp"

#include <stdexcept>
using namespace oreslang;
#define CHECK(cond) do { if (!(cond)) throw std::runtime_error("failed: " #cond); } while (false)

int main() {
    ores_actor_state_scope state{};
    state.owner_actor_id = 123;
    FunctionEffects declared{static_cast<std::uint32_t>(FunctionEffect::StateRead)
            | static_cast<std::uint32_t>(FunctionEffect::StateWrite)};
    CriticalStateCallback callback{};
    callback.state_mode = CriticalStateMode::Write;
    callback.callback_critical = true;
    callback.one_state_parameter = true;
    callback.inferred = declared;

    CHECK(ores_actor_state_ready_to_suspend(&state, 123) == ORES_ACTOR_STATE_OK);
    CHECK(ores_actor_state_ready_to_suspend(&state, 124) == ORES_ACTOR_STATE_WRONG_OWNER);
    CriticalStateLoan outer;
    CHECK(outer.begin(state, 123, 900, callback, declared)
            == CriticalStateAdmission::Accept);
    const auto outer_token = outer.token();
    CHECK(outer.check() == ORES_ACTOR_STATE_OK);
    CHECK(ores_actor_state_ready_to_suspend(&state, 123)
            == ORES_ACTOR_STATE_BUSY_LOAN);

    // Java forbids every nested sealed state loan. Native admission must
    // reject them independently of the frontend, including same-turn calls.
    CriticalStateLoan nested;
    CHECK(nested.begin(state, 123, 900, callback, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(nested.last_scope_result() == ORES_ACTOR_STATE_BUSY_LOAN);
    CHECK(!nested.active() && nested.token() == 0);
    CHECK(state.generation == outer_token);
    CHECK(outer.check() == ORES_ACTOR_STATE_OK);

    CriticalStateLoan other_continuation;
    CHECK(other_continuation.begin(state, 123, 901, callback, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(other_continuation.last_scope_result() == ORES_ACTOR_STATE_BUSY_LOAN);
    CHECK(!other_continuation.active());

    // Even a valid immutable READ outer scope cannot be escalated to WRITE
    // via a nested native call on the same logical continuation.
    auto read = callback;
    read.callback_critical = false;
    read.state_mode = CriticalStateMode::Read;
    read.inferred = {static_cast<std::uint32_t>(FunctionEffect::StateRead)};
    CHECK(outer.check() == ORES_ACTOR_STATE_OK);
    CHECK(outer.end() == ORES_ACTOR_STATE_OK);
    CHECK(ores_actor_state_ready_to_suspend(&state, 123)
            == ORES_ACTOR_STATE_OK);
    CriticalStateLoan outer_read;
    CHECK(outer_read.begin(state, 123, 900, read, declared)
            == CriticalStateAdmission::Accept);
    CriticalStateLoan upgraded;
    CHECK(upgraded.begin(state, 123, 900, callback, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(!upgraded.active());
    CHECK(outer_read.check() == ORES_ACTOR_STATE_OK);
    CHECK(outer_read.end() == ORES_ACTOR_STATE_OK);

    CHECK(outer.check() == ORES_ACTOR_STATE_STALE);
    CHECK(ores_actor_state_ready_to_suspend(&state, 123)
            == ORES_ACTOR_STATE_OK);

    // The exact same actor can next admit a *different* continuation.
    CriticalStateLoan resumed;
    CHECK(resumed.begin(state, 123, 901, callback, declared)
            == CriticalStateAdmission::Accept);
    CHECK(resumed.token() > outer_token);
    CHECK(resumed.end() == ORES_ACTOR_STATE_OK);

    // Fail closed on invalid source contracts, before state-loan allocation.
    auto forged_write = callback;
    forged_write.callback_critical = false;
    CriticalStateLoan forged;
    CHECK(forged.begin(state, 123, 901, forged_write, declared)
            == CriticalStateAdmission::MissingCallbackCritical);
    CHECK(!forged.active());

    auto missing = callback;
    missing.state_mode = CriticalStateMode::None;
    CriticalStateLoan invalid;
    CHECK(invalid.begin(state, 123, 901, missing, declared)
            == CriticalStateAdmission::MissingCallbackCritical);
    CHECK(!invalid.active());

    // An exception must revoke the ephemeral loan through stack unwinding.
    const auto previously_seen = state.generation;
    try {
        CriticalStateLoan unwound;
        CHECK(unwound.begin(state, 123, 902, callback, declared)
                == CriticalStateAdmission::Accept);
        throw std::runtime_error("user callback");
    } catch (const std::runtime_error&) {
        CHECK(state.depth == 0 && !state.active);
        CHECK(state.generation > previously_seen);
    }
    CHECK(ores_actor_state_ready_to_suspend(&state, 123) == ORES_ACTOR_STATE_OK);
    return 0;
}
