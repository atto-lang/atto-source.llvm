#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Runtime-only one-shot signed-i64 Future readiness source for static select.
 *
 * Observation is non-consuming: independent selects may observe the same
 * settled outcome. Each readiness subscription is still revocable and
 * identity-checked so stale notifications cannot win a select.
 */
template <std::size_t MaxWaiters = 64>
class NativeSelectableFutureI64 final {
    static_assert(MaxWaiters > 0, "future waiter capacity must be positive");

public:
    enum class OutcomeKind {
        Value,
        Cancelled,
    };

    struct Outcome final {
        OutcomeKind kind = OutcomeKind::Cancelled;
        std::int64_t value = 0;
    };

private:
    struct Waiter final {
        std::uint64_t id = 0;
        bool active = false;
        std::function<void()> notify;
    };

    struct State final {
        mutable std::mutex mutex;
        std::array<Waiter, MaxWaiters> waiters{};
        std::uint64_t next_waiter_id = 1;
        bool settled = false;
        Outcome outcome{};
        std::exception_ptr notification_failure;
    };

public:
    class Subscription final {
    public:
        Subscription() = default;

        bool close() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            if (index_ >= MaxWaiters) return false;
            auto& waiter = state->waiters[index_];
            if (!waiter.active || waiter.id != id_) return false;
            waiter.active = false;
            waiter.notify = {};
            return true;
        }

        bool active() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            if (index_ >= MaxWaiters) return false;
            const auto& waiter = state->waiters[index_];
            return waiter.active && waiter.id == id_;
        }

    private:
        friend class NativeSelectableFutureI64;

        Subscription(const std::shared_ptr<State>& state,
                     std::size_t index,
                     std::uint64_t id)
            : state_(state), index_(index), id_(id) {}

        std::weak_ptr<State> state_;
        std::size_t index_ = MaxWaiters;
        std::uint64_t id_ = 0;
    };

    NativeSelectableFutureI64()
        : state_(std::make_shared<State>()) {}

    NativeSelectableFutureI64(const NativeSelectableFutureI64&) = delete;
    NativeSelectableFutureI64& operator=(const NativeSelectableFutureI64&) = delete;
    NativeSelectableFutureI64(NativeSelectableFutureI64&&) = delete;
    NativeSelectableFutureI64& operator=(NativeSelectableFutureI64&&) = delete;

    template <class Callback>
    Subscription subscribe_ready(Callback&& callback) {
        std::function<void(const Subscription&)> consumer(
            std::forward<Callback>(callback));
        if (!consumer)
            throw std::invalid_argument("future readiness callback is empty");

        Subscription token;
        std::function<void()> immediate;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            std::size_t index = MaxWaiters;
            for (std::size_t i = 0; i < MaxWaiters; ++i) {
                if (!state_->waiters[i].active) {
                    index = i;
                    break;
                }
            }
            if (index == MaxWaiters)
                throw std::overflow_error(
                    "future select waiter capacity exhausted");
            if (state_->next_waiter_id ==
                std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error(
                    "future select waiter identity exhausted");

            const auto id = state_->next_waiter_id++;
            token = Subscription(state_, index, id);
            std::function<void()> wrapped(
                [consumer = std::move(consumer), token]() mutable {
                    consumer(token);
                });

            auto& waiter = state_->waiters[index];
            waiter.id = id;
            waiter.active = true;
            waiter.notify = wrapped;
            if (state_->settled) immediate = std::move(wrapped);
        }

        if (immediate) {
            try {
                immediate();
            } catch (...) {
                (void)token.close();
                throw;
            }
        }
        return token;
    }

    bool try_commit(const Subscription& subscription, Outcome& outcome) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if (!matches_locked(subscription) || !state_->settled)
            return false;

        outcome = state_->outcome;
        close_locked(state_->waiters[subscription.index_]);
        return true;
    }

    bool resolve(std::int64_t value) {
        Outcome outcome;
        outcome.kind = OutcomeKind::Value;
        outcome.value = value;
        return settle(outcome);
    }

    bool cancel() {
        Outcome outcome;
        outcome.kind = OutcomeKind::Cancelled;
        outcome.value = 0;
        return settle(outcome);
    }

    bool settled() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->settled;
    }

    std::size_t pending_waiters() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        std::size_t count = 0;
        for (const auto& waiter : state_->waiters)
            if (waiter.active) ++count;
        return count;
    }

    bool has_notification_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return bool(state_->notification_failure);
    }

private:
    bool matches_locked(const Subscription& subscription) const {
        auto owner = subscription.state_.lock();
        if (!owner || owner.get() != state_.get() ||
            subscription.index_ >= MaxWaiters)
            return false;
        const auto& waiter = state_->waiters[subscription.index_];
        return waiter.active && waiter.id == subscription.id_;
    }

    static void close_locked(Waiter& waiter) noexcept {
        waiter.active = false;
        waiter.notify = {};
    }

    bool settle(const Outcome& outcome) {
        std::array<std::function<void()>, MaxWaiters> callbacks{};
        std::size_t callback_count = 0;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->settled) return false;

            // Capture every callback before publishing terminal state. If a
            // std::function copy allocates and throws, the Future remains
            // pending and retryable.
            for (const auto& waiter : state_->waiters) {
                if (waiter.active && waiter.notify)
                    callbacks[callback_count++] = waiter.notify;
            }
            state_->settled = true;
            state_->outcome = outcome;
        }

        std::exception_ptr first_failure;
        for (std::size_t i = 0; i < callback_count; ++i) {
            try {
                callbacks[i]();
            } catch (...) {
                if (!first_failure)
                    first_failure = std::current_exception();
            }
        }

        if (first_failure) {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (!state_->notification_failure)
                state_->notification_failure = std::move(first_failure);
        }
        return true;
    }

    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
