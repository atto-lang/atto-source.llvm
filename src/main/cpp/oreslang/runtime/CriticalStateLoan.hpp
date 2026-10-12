#pragma once

#include "oreslang/runtime/ActorStateScope.h"
#include "oreslang/types/CriticalHandleStateContract.hpp"

#include <cstdint>
#include <exception>

namespace oreslang {

/**
 * Trusted native compiler/runtime bridge: validate the critical callback
 * effect contract AND acquire a real generation-fenced scoped state loan.
 *
 * Does not mint scheduler continuation authority: actor_id and
 * continuation_id MUST be verified upstream by the actor turn executor.
 * No native thread lock is held. No future/await completion is run here.
 */
class CriticalStateLoan final {
public:
    CriticalStateLoan() = default;
    CriticalStateLoan(const CriticalStateLoan&) = delete;
    CriticalStateLoan& operator=(const CriticalStateLoan&) = delete;
    CriticalStateLoan(CriticalStateLoan&&) = delete;
    CriticalStateLoan& operator=(CriticalStateLoan&&) = delete;

    CriticalStateAdmission begin(ores_actor_state_scope& state,
                                 std::uint64_t actor_id,
                                 std::uint64_t continuation_id,
                                 const CriticalStateCallback& callback,
                                 FunctionEffects declared_using,
                                 bool verified_serial_actor = false) noexcept {
        if (active_) return CriticalStateAdmission::InvalidStateCallback;
        const auto approved = admit_critical_handle_state(
                callback, declared_using, verified_serial_actor);
        if (approved != CriticalStateAdmission::Accept) return approved;
        const auto entered = ores_actor_state_enter_continuation(
                &state, actor_id, continuation_id, &token_);
        last_scope_result_ = entered;
        if (entered != ORES_ACTOR_STATE_OK) return CriticalStateAdmission::ScopeDenied;
        owner_ = &state;
        actor_id_ = actor_id;
        continuation_id_ = continuation_id;
        active_ = true;
        return CriticalStateAdmission::Accept;
    }

    ores_actor_state_result check() const noexcept {
        if (!active_ || owner_ == nullptr) return ORES_ACTOR_STATE_STALE;
        return ores_actor_state_check_continuation(
                owner_, actor_id_, continuation_id_, token_);
    }

    ores_actor_state_result end() noexcept {
        if (!active_ || !owner_) return ORES_ACTOR_STATE_STALE;
        const auto result = ores_actor_state_leave_continuation(
                owner_, actor_id_, continuation_id_, token_);
        if (result == ORES_ACTOR_STATE_OK) {
            active_ = false;
            token_ = 0;
            owner_ = nullptr;
        }
        last_scope_result_ = result;
        return result;
    }

    ores_actor_state_result last_scope_result() const noexcept {
        return last_scope_result_;
    }

    bool active() const noexcept { return active_; }
    std::uint64_t token() const noexcept { return token_; }

    ~CriticalStateLoan() noexcept {
        // Automatic unwinding enforces loan retirement after exceptions.
        // An out-of-order destruction is an invariant violation; continuing
        // would leak actor admission authority into a later continuation.
        if (active_ && end() != ORES_ACTOR_STATE_OK) std::terminate();
    }

private:
    ores_actor_state_scope* owner_ = nullptr;
    std::uint64_t actor_id_ = 0;
    std::uint64_t continuation_id_ = 0;
    std::uint64_t token_ = 0;
    ores_actor_state_result last_scope_result_ = ORES_ACTOR_STATE_STALE;
    bool active_ = false;
};

} // namespace oreslang
