#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Actor-generation lease used to fence compiler-lowered continuations.
 *
 * Lock order for select adapters:
 *   select arbiter -> generation fence -> one readiness source
 *
 * A successful try_with_current() holds the generation stable for the entire
 * bounded commit callback. advance_generation() must run without select,
 * channel, or Future locks.
 */
class NativeActorGenerationFence final {
private:
    struct State final {
        mutable std::recursive_mutex mutex;
        std::uint64_t generation = 1;
        bool commit_in_progress = false;
    };

public:
    class Lease final {
    public:
        Lease() = default;

        std::uint64_t generation() const noexcept {
            return generation_;
        }

        bool current() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::recursive_mutex> guard(state->mutex);
            return state->generation == generation_;
        }

        template <class Commit>
        bool try_with_current(Commit&& commit) const {
            auto state = state_.lock();
            if (!state) return false;

            std::lock_guard<std::recursive_mutex> guard(state->mutex);
            if (state->generation != generation_) return false;
            if (state->commit_in_progress)
                throw std::logic_error(
                    "generation commit must not re-enter its fence");

            state->commit_in_progress = true;
            try {
                const bool committed = static_cast<bool>(
                    std::forward<Commit>(commit)());
                state->commit_in_progress = false;
                return committed;
            } catch (...) {
                state->commit_in_progress = false;
                throw;
            }
        }

    private:
        friend class NativeActorGenerationFence;

        Lease(const std::shared_ptr<State>& state, std::uint64_t generation)
            : state_(state), generation_(generation) {}

        std::weak_ptr<State> state_;
        std::uint64_t generation_ = 0;
    };

    NativeActorGenerationFence()
        : state_(std::make_shared<State>()) {}

    NativeActorGenerationFence(const NativeActorGenerationFence&) = delete;
    NativeActorGenerationFence& operator=(const NativeActorGenerationFence&) = delete;
    NativeActorGenerationFence(NativeActorGenerationFence&&) = delete;
    NativeActorGenerationFence& operator=(NativeActorGenerationFence&&) = delete;

    Lease capture() const {
        std::lock_guard<std::recursive_mutex> guard(state_->mutex);
        return Lease(state_, state_->generation);
    }

    std::uint64_t current_generation() const {
        std::lock_guard<std::recursive_mutex> guard(state_->mutex);
        return state_->generation;
    }

    std::uint64_t advance_generation() {
        std::lock_guard<std::recursive_mutex> guard(state_->mutex);
        if (state_->commit_in_progress)
            throw std::logic_error(
                "actor generation cannot advance inside a fenced commit");
        if (state_->generation ==
            std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("actor generation exhausted");
        return ++state_->generation;
    }

private:
    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
