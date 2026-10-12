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

    CriticalStateLoan nested;
    CHECK(nested.begin(state, 123, 900, callback, declared)
            == CriticalStateAdmission::Accept);
    CHECK(nested.token() != outer_token);
    CHECK(outer.check() == ORES_ACTOR_STATE_OUT_OF_ORDER);
    CHECK(nested.check() == ORES_ACTOR_STATE_OK);
    CHECK(outer.end() == ORES_ACTOR_STATE_OUT_OF_ORDER);
    CHECK(nested.end() == ORES_ACTOR_STATE_OK);
    CHECK(outer.check() == ORES_ACTOR_STATE_OK);

    CriticalStateLoan other_continuation;
    CHECK(other_continuation.begin(state, 123, 901, callback, declared)
            == CriticalStateAdmission::ScopeDenied);
    CHECK(other_continuation.last_scope_result() == ORES_ACTOR_STATE_REENTRANT);
    CHECK(!other_continuation.active());

    CHECK(outer.end() == ORES_ACTOR_STATE_OK);
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
