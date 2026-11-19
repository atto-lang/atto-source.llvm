#pragma once

#include "oreslang/runtime/SupervisorFailureLane.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

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
    enum class Decision : std::uint8_t { Acknowledge, Escalate, Restart, Stop };
    // An explicit host receipt is required. Uncertain means the operation
    // MAY have occurred; never implicitly retry or acknowledge its child.
    enum class StopReceipt : std::uint8_t { Acknowledged, Uncertain };
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
        // Default fails closed for existing hosts without an authenticated
        // STOP implementation. Must only return Acknowledged after physical
        // finalization and authoritative host receipt.
        virtual StopReceipt on_stop(const SupervisorFailureLane::Failure&) {
            return StopReceipt::Uncertain;
        }
        virtual void on_user_turn() = 0;
    };

    explicit SupervisorControlTurn(std::size_t capacity,
                                   std::size_t restart_budget = 0,
                                   std::uint64_t restart_window_ticks = 0)
        : child_failures_(capacity),
          restart_budget_(checked_restart_budget(restart_budget)),
          restart_window_ticks_(restart_window_ticks),
          restart_ticks_(restart_budget_, 0) {}

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

    // A stopped child remains in-flight and keeps its reserved capacity when
    // the STOP effect is uncertain. No subsequent turns may run until the
    // trusted owner reconciles that exact actor incarnation.
    // Observers can run concurrently with the owner CONTROL turn. Do not
    // read the mutable optional outside its exclusive lease (C++ data race).
    bool stop_quarantined() const noexcept {
        return stop_quarantined_.load(std::memory_order_acquire);
    }

    void confirm_stopped(const SupervisorFailureLane::ActorKey& child) {
        if (lease_.test_and_set(std::memory_order_acquire)) {
            throw std::logic_error("overlapping native supervisor STOP reconciliation");
        }
        struct Release {
            std::atomic_flag& lease;
            ~Release() { lease.clear(std::memory_order_release); }
        } release{lease_};
        if (!stop_pending_ || stop_pending_->id != child.id
                || stop_pending_->generation != child.generation) {
            throw std::logic_error("unmatched native supervisor STOP receipt");
        }
        // Only trusted host code may assert authoritative STOP acknowledgment.
        child_failures_.ack(child);
        stop_pending_.reset();
        stop_quarantined_.store(false, std::memory_order_release);
    }

    std::size_t pending_count() const { return child_failures_.pending_count(); }
    std::size_t reserved_count() const { return child_failures_.reserved_count(); }
    // These counters may be sampled from independent host telemetry threads
    // while the supervisor holds its exclusive CONTROL turn. Plain size_t
    // reads would race with consume_restart_slot() and are undefined in C++.
    std::size_t restarts_used() const noexcept {
        return restarts_used_.load(std::memory_order_relaxed);
    }
    std::size_t restarts_in_window() const noexcept {
        return restart_count_.load(std::memory_order_relaxed);
    }

    /**
     * At most one CONTROL event per admitted turn. CONTROL takes priority
     * over continuations and user mail; invoke this at the top of the actor
     * scheduler's existing single-flight turn before ordinary work.
     */
    Admitted admit_turn(Hooks& hooks, bool ordinary_mail_ready) {
        return admit_turn(hooks, ordinary_mail_ready, 0);
    }

    /**
     * Window-aware CONTROL admission. restart_tick is a caller-supplied
     * monotonic scheduler tick; no clock, thread or timer is created here.
     * A zero restart_window_ticks constructor preserves the legacy lifetime
     * restart budget. Windowed callers must provide nondecreasing ticks.
     */
    Admitted admit_turn(Hooks& hooks, bool ordinary_mail_ready,
                        std::uint64_t restart_tick) {
        if (lease_.test_and_set(std::memory_order_acquire)) {
            throw std::logic_error("overlapping native supervisor logical turns");
        }
        struct Release {
            std::atomic_flag& lease;
            ~Release() { lease.clear(std::memory_order_release); }
        } release{lease_};

        if (stop_pending_) {
            throw std::logic_error("native supervisor STOP uncertain; reconcile before admitting a turn");
        }
        const auto next = child_failures_.poll();
        if (next) {
            bool stop_may_have_effects = false;
            try {
                const auto decision = dispatch(hooks, *next);
                switch (decision) {
                    case Decision::Acknowledge:
                        break;
                    case Decision::Escalate:
                        hooks.on_escalate(*next);
                        break;
                    case Decision::Stop:
                        // Save the entire generation BEFORE calling an effectful
                        // host. If allocation fails here, the host was not called
                        // and ordinary CONTROL retry remains safe.
                        stop_pending_ = next->actor;
                        // Publish quarantine before any potentially effectful
                        // host callback, including callbacks on foreign threads.
                        stop_quarantined_.store(true, std::memory_order_release);
                        stop_may_have_effects = true;
                        if (hooks.on_stop(*next) != StopReceipt::Acknowledged) {
                            // Hold the event in-flight and the child slot
                            // reserved. A missing/uncertain receipt must not
                            // cause STOP re-execution or false resolution.
                            return Admitted::Control;
                        }
                        break;
                    case Decision::Restart:
                        // Restart admission is supervisor-scoped. With a
                        // configured window this is an intensity bound rather
                        // than a permanent lifetime fuse. A failed restart
                        // attempt still consumes its slot: retry storms must
                        // never bypass supervision policy.
                        if (!consume_restart_slot(restart_tick)) {
                            hooks.on_escalate(*next);
                        } else if (!hooks.on_restart(*next)) {
                            hooks.on_escalate(*next);
                        }
                        break;
                    default:
                        // Invalid FFI/hook decision must escalate in this
                        // same CONTROL turn: endless retries would strand the
                        // pre-reserved child failure and allow no progress.
                        // An exception thrown by escalation itself is still
                        // retried through the existing catch/retry boundary.
                        hooks.on_escalate(*next);
                        break;
                }
                child_failures_.ack(next->actor);
                if (stop_may_have_effects) {
                    stop_pending_.reset();
                    stop_quarantined_.store(false, std::memory_order_release);
                }
            } catch (...) {
                // An effectful STOP may have succeeded before throwing:
                // quarantine until the trusted host proves final disposition.
                if (!stop_may_have_effects) child_failures_.retry(next->actor);
                throw;
            }
            return Admitted::Control;
        }
        if (!ordinary_mail_ready) return Admitted::None;
        hooks.on_user_turn();
        return Admitted::User;
    }

private:
    static std::size_t checked_restart_budget(std::size_t budget) {
        // A malformed host CONTROL policy must fail before constructing an
        // unbounded restart history vector. Restart is a policy quota, not
        // permission to allocate SIZE_MAX bytes on supervisor admission.
        constexpr std::size_t max_restart_history = 1024;
        if (budget > max_restart_history)
            throw std::invalid_argument("native supervisor restart budget exceeds 1024");
        return budget;
    }

    bool consume_restart_slot(std::uint64_t now) noexcept {
        if (restart_budget_ == 0) return false;
        if (restart_window_ticks_ == 0) {
            if (restarts_used_ >= restart_budget_) return false;
            ++restarts_used_;
            return true;
        }

        // A regressing scheduler tick is not trustworthy restart evidence.
        // Fail closed by denying this restart rather than throwing/retrying
        // the same CONTROL event forever.
        if (restart_count_ != 0 && now < last_restart_tick_) return false;

        while (restart_count_ != 0) {
            const std::uint64_t oldest = restart_ticks_[restart_head_];
            if (now < oldest || now - oldest < restart_window_ticks_) break;
            restart_head_ = (restart_head_ + 1) % restart_budget_;
            --restart_count_;
        }
        if (restart_count_ >= restart_budget_) return false;

        const std::size_t index =
                (restart_head_ + restart_count_) % restart_budget_;
        restart_ticks_[index] = now;
        ++restart_count_;
        last_restart_tick_ = now;
        ++restarts_used_;
        return true;
    }

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
    const std::uint64_t restart_window_ticks_;
    std::vector<std::uint64_t> restart_ticks_;
    std::size_t restart_head_ = 0;
    std::atomic<std::size_t> restart_count_{0};
    std::uint64_t last_restart_tick_ = 0;
    std::atomic<std::size_t> restarts_used_{0}; // one CONTROL writer; concurrent host observers
    std::atomic_flag lease_ = ATOMIC_FLAG_INIT;
    // Supervisor logical-turn owned, not a guest ActorRef or public token.
    // Retained on uncertain or throwing STOP effects for explicit reconciliation.
    std::optional<SupervisorFailureLane::ActorKey> stop_pending_;
    std::atomic<bool> stop_quarantined_{false};

};

} // namespace oreslang
