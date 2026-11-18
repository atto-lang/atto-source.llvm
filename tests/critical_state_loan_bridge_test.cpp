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

    // A sealed state callback may not nest within the same actor turn.
    // Reject BEFORE entering the low-level token stack: generation and outer
    // read/write authority must remain unmodified, even for read/read.
    CriticalStateLoan nested;
    CHECK(nested.begin(state, 123, 900, callback, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(nested.last_scope_result() == ORES_ACTOR_STATE_BUSY_LOAN);
    CHECK(!nested.active() && nested.token() == 0);
    CHECK(state.generation == outer_token && state.depth == 1);
    CHECK(outer.check() == ORES_ACTOR_STATE_OK);

    // A foreign continuation cannot hijack the existing state loan either.
    CriticalStateLoan other_continuation;
    CHECK(other_continuation.begin(state, 123, 901, callback, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(other_continuation.last_scope_result() == ORES_ACTOR_STATE_BUSY_LOAN);
    CHECK(!other_continuation.active());
    CHECK(outer.check() == ORES_ACTOR_STATE_OK);
    auto read_inside_write = callback;
    read_inside_write.inferred = {static_cast<std::uint32_t>(
        FunctionEffect::StateRead)};
    read_inside_write.callback_critical = false;
    CriticalStateLoan nested_write_to_read;
    CHECK(nested_write_to_read.begin(state, 123, 900,
                                   read_inside_write, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(!nested_write_to_read.active() && nested_write_to_read.token() == 0);
    CHECK(outer.check() == ORES_ACTOR_STATE_OK);
    CHECK(outer.end() == ORES_ACTOR_STATE_OK);
    CHECK(outer.check() == ORES_ACTOR_STATE_STALE);
    CHECK(ores_actor_state_ready_to_suspend(&state, 123)
            == ORES_ACTOR_STATE_OK);

    // A READ-only outer loan cannot be upgraded to WRITE by nesting.
    auto read_only = callback;
    read_only.callback_critical = false;
    read_only.inferred = {static_cast<std::uint32_t>(
        FunctionEffect::StateRead)};
    CriticalStateLoan outer_read;
    CHECK(outer_read.begin(state, 123, 900, read_only, declared)
            == CriticalStateAdmission::Accept);
    const auto read_generation = state.generation;
    // READ -> READ must also be denied: even immutable nested callbacks
    // may not escape the same lexical state-scoped transaction.
    CriticalStateLoan nested_read;
    CHECK(nested_read.begin(state, 123, 900, read_only, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(nested_read.last_scope_result() == ORES_ACTOR_STATE_BUSY_LOAN);
    CHECK(!nested_read.active() && nested_read.token() == 0);
    CriticalStateLoan upgraded;
    CHECK(upgraded.begin(state, 123, 900, callback, declared)
            == CriticalStateAdmission::NestedStateLoan);
    CHECK(upgraded.last_scope_result() == ORES_ACTOR_STATE_BUSY_LOAN);
    CHECK(!upgraded.active());
    CHECK(state.generation == read_generation && state.depth == 1);
    CHECK(outer_read.check() == ORES_ACTOR_STATE_OK);
    CHECK(outer_read.end() == ORES_ACTOR_STATE_OK);
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
    missing.callback_critical = false;
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
