#pragma once

#include "oreslang/runtime/SupervisorFailureLane.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace oreslang {

/**
 * Native supervisor admission protocol.
 *
 * The native scheduler calls admit_turn() only while holding the supervisor's
 * existing logical actor-turn authority. This additional atomic lease catches
 * accidental overlapping callbacks, including when physical carriers change.
 *
 * It is not a substitute for native actor lowering or a scheduler backend.
 * Child carriers only reserve/publish/wake; guest hooks run in admit_turn().
 */
class SupervisorControlTurn final {
public:
    enum class Decision : std::uint8_t { Acknowledge, Escalate, Restart };
    enum class Admitted : std::uint8_t { None, Control, User };

    struct Hooks {
        virtual ~Hooks() = default;
        virtual Decision on_throw(const SupervisorFailureLane::Failure&) = 0;
        virtual Decision on_raise(const SupervisorFailureLane::Failure&) = 0;
        virtual Decision on_panic(const SupervisorFailureLane::Failure&) = 0;
        virtual Decision on_cancel(const SupervisorFailureLane::Failure&) = 0;
        virtual Decision on_host_failure(const SupervisorFailureLane::Failure&) = 0;
        virtual void on_escalate(const SupervisorFailureLane::Failure&) = 0;
        virtual bool on_restart(const SupervisorFailureLane::Failure&) = 0;
        virtual void on_user_turn() = 0;
    };

    explicit SupervisorControlTurn(std::size_t capacity,
                                   std::size_t restart_budget = 0)
        : child_failures_(capacity), restart_budget_(restart_budget) {}

    void reserve_child(const SupervisorFailureLane::ActorKey& child) {
        child_failures_.reserve(child);
    }

    // Invoked on child termination; returns true exactly once per generation.
    // Native scheduler must enqueue an urgent parent admission after true.
    // This is an actor terminal boundary: raw Throw/Raise are never emitted
    // as CONTROL event kinds. Preserve their pre-promotion origin for policy.
    // Already-promoted Panic(origin=Throw/Raise) is allowed on escalation.
    bool publish_child(const SupervisorFailureLane::ActorKey& child,
                       SupervisorFailureLane::Kind kind,
                       std::string_view summary,
                       std::string_view parent_id = {},
                       std::optional<SupervisorFailureLane::Kind> origin = std::nullopt,
                       std::uint32_t origin_code = 0) {
        using Kind = SupervisorFailureLane::Kind;
        if (kind == Kind::Throw || kind == Kind::Raise) {
            // Mismatched supplied provenance must not be silently rewritten.
            if (origin && *origin != kind) {
                throw std::invalid_argument("actor terminal origin contradicts effect");
            }
            origin = kind;
            kind = Kind::Panic;
        }
        const bool admitted = child_failures_.publish_preallocated(
            child, kind, summary, parent_id, origin, origin_code);
        return admitted;
    }

    void retire_child(const SupervisorFailureLane::ActorKey& child) {
        child_failures_.retire_gracefully(child);
    }

    // Linearize readiness with publish_child() under the lane mutex.
    // A cached atomic ready bit can be cleared by a consumer *after* a
    // concurrent producer sets it, stranding a pending CONTROL event.
    bool urgent() const {
        return child_failures_.pending_count() != 0;
    }

    std::size_t pending_count() const { return child_failures_.pending_count(); }
    std::size_t reserved_count() const { return child_failures_.reserved_count(); }
    std::size_t restarts_used() const noexcept { return restarts_used_; }

    /**
     * At most one CONTROL event per admitted turn. CONTROL takes priority
     * over continuations and user mail; invoke this at the top of the actor
     * scheduler's existing single-flight turn before ordinary work.
     */
    Admitted admit_turn(Hooks& hooks, bool ordinary_mail_ready) {
        if (lease_.test_and_set(std::memory_order_acquire)) {
            throw std::logic_error("overlapping native supervisor logical turns");
        }
        struct Release {
            std::atomic_flag& lease;
            ~Release() { lease.clear(std::memory_order_release); }
        } release{lease_};

        const auto next = child_failures_.poll();
        if (next) {
            try {
                const auto decision = dispatch(hooks, *next);
                switch (decision) {
                    case Decision::Acknowledge:
                        break;
                    case Decision::Escalate:
                        hooks.on_escalate(*next);
                        break;
                    case Decision::Restart:
                        // Budget is supervisor-scoped. A failed attempt must
                        // escalate; no actor generation may be silently reused.
                        if (restarts_used_ >= restart_budget_) {
                            hooks.on_escalate(*next);
                        } else {
                            ++restarts_used_;
                            if (!hooks.on_restart(*next)) hooks.on_escalate(*next);
                        }
                        break;
                    default:
                        // Invalid enum tags from host FFI/corrupt handlers must
                        // retain the in-flight CONTROL event for retry rather
                        // than silently acknowledging a failed actor.
                        throw std::invalid_argument("invalid supervisor CONTROL decision");
                }
                child_failures_.ack(next->actor);
            } catch (...) {
                child_failures_.retry(next->actor);
                throw;
            }
            return Admitted::Control;
        }
        if (!ordinary_mail_ready) return Admitted::None;
        hooks.on_user_turn();
        return Admitted::User;
    }

private:
    static Decision dispatch(Hooks& hooks,
                             const SupervisorFailureLane::Failure& failure) {
        switch (failure.kind) {
            case SupervisorFailureLane::Kind::Throw: return hooks.on_throw(failure);
            case SupervisorFailureLane::Kind::Raise: return hooks.on_raise(failure);
            case SupervisorFailureLane::Kind::Panic: return hooks.on_panic(failure);
            case SupervisorFailureLane::Kind::Cancel: return hooks.on_cancel(failure);
            case SupervisorFailureLane::Kind::HostFailure:
                return hooks.on_host_failure(failure);
        }
        throw std::logic_error("unknown native effect kind");
    }

    SupervisorFailureLane child_failures_;
    const std::size_t restart_budget_;
    std::size_t restarts_used_ = 0; // single-writer supervisor turn
    std::atomic_flag lease_ = ATOMIC_FLAG_INIT;

};

} // namespace oreslang
