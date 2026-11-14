#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Runtime-owned receive-invocation gate for one physical actor incarnation.
 *
 * Independent from NativeActorGroup's existing single-writer turn lease:
 * future concurrent source lowering must retain one ReceiveLease for EACH
 * live invocation (including suspended continuations) and use this gate at
 * actual admission. This is not an actor scheduler or a guest hook.
 *
 * All operations are bounded, use one preallocated shared State, and never
 * invoke guest code or a destructor under the mutex. No tracing collector or
 * reclamation worker is involved.
 */
class NativeActorReceiveFence final {
private:
    struct State {
        mutable std::mutex mutex;
        const std::uint64_t actor;
        const std::uint64_t incarnation;
        const std::size_t limit;
        std::size_t active = 0;
        bool stopped = false;
        bool graceful = false;
        bool teardown_started = false;
        bool teardown_active = false;
        bool teardown_finished = false;

        State(std::uint64_t id, std::uint64_t generation, std::size_t max_active)
            : actor(id), incarnation(generation), limit(max_active) {}
    };

public:
    class ReceiveLease final {
    public:
        ReceiveLease(const ReceiveLease&) = delete;
        ReceiveLease& operator=(const ReceiveLease&) = delete;
        ReceiveLease(ReceiveLease&&) noexcept = default;
        ReceiveLease& operator=(ReceiveLease&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_);
            }
            return *this;
        }
        ~ReceiveLease() { release(); }

        void release() noexcept {
            auto owned = std::move(state_);
            if (!owned) return;
            std::lock_guard<std::mutex> lock(owned->mutex);
            if (owned->active == 0) std::terminate(); // corrupt trusted receipt
            --owned->active;
        }

    private:
        friend class NativeActorReceiveFence;
        explicit ReceiveLease(std::shared_ptr<State> state) noexcept
            : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };

    NativeActorReceiveFence(std::uint64_t actor, std::uint64_t incarnation,
                            std::size_t max_active)
        : state_(std::make_shared<State>(actor, incarnation, max_active)) {
        if (max_active == 0 || max_active > 4096)
            throw std::invalid_argument("native actor receive limit out of range");
    }

    NativeActorReceiveFence(const NativeActorReceiveFence&) = delete;
    NativeActorReceiveFence& operator=(const NativeActorReceiveFence&) = delete;

    std::uint64_t actor_id() const noexcept { return state_->actor; }
    std::uint64_t incarnation() const noexcept { return state_->incarnation; }

    /** STOP versus new receive admission linearizes under the same lock.
     * A failed admission retains no lease and consumes no quota. */
    std::optional<ReceiveLease> try_enter(std::uint64_t actor,
                                          std::uint64_t incarnation) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (actor != state_->actor || incarnation != state_->incarnation)
            throw std::invalid_argument("native actor receive identity mismatch");
        if (state_->stopped || state_->active >= state_->limit) return std::nullopt;
        ++state_->active;
        return std::optional<ReceiveLease>(ReceiveLease(state_));
    }

    /** A later force/cancel can revoke graceful teardown, never upgrade it. */
    void seal_stop(bool graceful) noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->stopped) {
            state_->stopped = true;
            state_->graceful = graceful;
        } else if (!graceful) {
            state_->graceful = false;
        }
    }

    /** Exclusive lifecycle entry only after all receiver leases retire. */
    bool try_begin_teardown() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->stopped || !state_->graceful || state_->active != 0
                || state_->teardown_started) return false;
        state_->teardown_started = true;
        state_->teardown_active = true;
        return true;
    }

    /** Must be called even after a failed cleanup callback; the trusted
     * resource registry separately accounts for failure/quarantine. */
    void finish_teardown() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->teardown_active || state_->teardown_finished)
            throw std::logic_error("native actor teardown completion without active hook");
        state_->teardown_active = false;
        state_->teardown_finished = true;
    }

    /** Physical release is forbidden while a receive or lifecycle callback
     * can still access actor memory. Other native loans, tasks, FFI pins, and
     * group carrier leases are checked by NativeActorRetirementBarrier. */
    bool ready_for_reclamation() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->stopped && state_->active == 0
            && !state_->teardown_active
            && (!state_->graceful || state_->teardown_finished);
    }

    std::size_t active_receives() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->active;
    }

private:
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
