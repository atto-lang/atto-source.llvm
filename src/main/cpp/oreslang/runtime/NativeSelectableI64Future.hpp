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

template <std::size_t MaxWaiters = 64>
class NativeSelectableI64Future final {
    static_assert(MaxWaiters > 0, "future waiter capacity must be positive");

public:
    enum class Status { Pending, Value, Failed, Cancelled };
    struct Result {
        Status status = Status::Pending;
        std::int64_t value = 0;
        std::exception_ptr failure;
    };
    class Subscription;

private:
    struct Waiter {
        std::uint64_t id = 0;
        bool active = false;
        bool retain_terminal = false;
        std::function<void(const Subscription&)> notify;
    };
    struct State {
        mutable std::mutex mutex;
        std::array<Waiter, MaxWaiters> waiters{};
        std::uint64_t next_id = 1;
        Status status = Status::Pending;
        std::int64_t value = 0;
        std::exception_ptr failure;
        std::exception_ptr notification_failure;
    };

public:
    class Subscription final {
    public:
        Subscription() = default;
        bool close() const {
            auto s = state_.lock();
            if (!s) return false;
            std::lock_guard<std::mutex> guard(s->mutex);
            if (index_ >= MaxWaiters) return false;
            auto& w = s->waiters[index_];
            if (!w.active || w.id != id_) return false;
            w.active = false;
            w.notify = {};
            return true;
        }
        bool active() const {
            auto s = state_.lock();
            if (!s) return false;
            std::lock_guard<std::mutex> guard(s->mutex);
            if (index_ >= MaxWaiters) return false;
            const auto& w = s->waiters[index_];
            return w.active && w.id == id_;
        }
    private:
        friend class NativeSelectableI64Future;
        Subscription(const std::shared_ptr<State>& s,
                     std::size_t index,
                     std::uint64_t id)
            : state_(s), index_(index), id_(id) {}
        std::weak_ptr<State> state_;
        std::size_t index_ = MaxWaiters;
        std::uint64_t id_ = 0;
    };

    NativeSelectableI64Future() : state_(std::make_shared<State>()) {}

    // Copyable Future handles alias the same producer state.
    bool valid_source() const noexcept { return bool(state_); }
    bool same_source(const NativeSelectableI64Future& other) const noexcept {
        return state_ == other.state_;
    }

    template <class Callback>
    Subscription subscribe_ready(Callback&& callback) {
        return subscribe_ready_impl(std::forward<Callback>(callback), false);
    }

    template <class Callback>
    Subscription subscribe_ready_advisory(Callback&& callback) {
        return subscribe_ready_impl(std::forward<Callback>(callback), true);
    }

private:
    template <class Callback>
    Subscription subscribe_ready_impl(Callback&& callback, bool retain_terminal) {
        std::function<void(const Subscription&)> fn(
            std::forward<Callback>(callback));
        if (!fn) throw std::invalid_argument("future callback is empty");

        Subscription sub;
        bool immediate = false;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            std::size_t index = MaxWaiters;
            for (std::size_t i = 0; i < MaxWaiters; ++i)
                if (!state_->waiters[i].active) { index = i; break; }
            if (index == MaxWaiters)
                throw std::overflow_error("future waiter capacity exhausted");
            if (state_->next_id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("future waiter identity exhausted");
            const auto id = state_->next_id;
            auto& w = state_->waiters[index];

            // std::function copy may allocate and throw. Publish waiter
            // identity/activeness only after the callback copy succeeds so
            // subscription admission is failure-atomic and cannot consume an
            // unusable slot.
            w.notify = fn;
            w.retain_terminal = retain_terminal;
            w.id = id;
            w.active = true;
            ++state_->next_id;
            sub = Subscription(state_, index, id);
            immediate = state_->status != Status::Pending;
        }
        if (immediate) {
            try {
                fn(sub);
                // A terminal Future has no later readiness transition. If the
                // callback observed readiness without consuming the waiter,
                // retire it now rather than leaving a permanently inert slot.
                if (!retain_terminal) (void)sub.close();
            } catch (...) {
                (void)sub.close();
                throw;
            }
        }
        return sub;
    }

public:
    bool try_commit(const Subscription& sub, Result& result) {
        std::lock_guard<std::mutex> guard(state_->mutex);
        result = {};
        if (!matches_locked(sub) || state_->status == Status::Pending)
            return false;
        auto& w = state_->waiters[sub.index_];
        w.active = false;
        w.notify = {};
        result.status = state_->status;
        result.value = state_->value;
        result.failure = state_->failure;
        return true;
    }

    bool resolve(std::int64_t value) {
        return complete(Status::Value, value, {});
    }
    bool fail(std::exception_ptr failure) {
        if (!failure) throw std::invalid_argument("future failure is empty");
        return complete(Status::Failed, 0, std::move(failure));
    }
    bool cancel() { return complete(Status::Cancelled, 0, {}); }

    Status status() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->status;
    }
    std::size_t pending_waiters() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        std::size_t n = 0;
        for (const auto& w : state_->waiters) if (w.active) ++n;
        return n;
    }
    bool has_notification_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return bool(state_->notification_failure);
    }
    std::exception_ptr notification_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->notification_failure;
    }

private:
    bool matches_locked(const Subscription& sub) const {
        auto owner = sub.state_.lock();
        if (!owner || owner.get() != state_.get() || sub.index_ >= MaxWaiters)
            return false;
        const auto& w = state_->waiters[sub.index_];
        return w.active && w.id == sub.id_;
    }

    bool complete(Status status,
                  std::int64_t value,
                  std::exception_ptr failure) {
        std::array<std::function<void(const Subscription&)>, MaxWaiters> fns{};
        std::array<Subscription, MaxWaiters> subs{};
        std::array<bool, MaxWaiters> retain_terminal{};
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->status != Status::Pending) return false;
            for (std::size_t i = 0; i < MaxWaiters; ++i) {
                const auto& w = state_->waiters[i];
                if (!w.active || !w.notify) continue;
                fns[count] = w.notify;
                subs[count] = Subscription(state_, i, w.id);
                retain_terminal[count] = w.retain_terminal;
                ++count;
            }
            state_->status = status;
            state_->value = value;
            state_->failure = std::move(failure);
        }

        std::exception_ptr first;
        for (std::size_t i = 0; i < count; ++i) {
            try {
                fns[i](subs[i]);
                // Completion is immutable. A callback may consume its waiter
                // via try_commit(), lose a select and be revoked by the
                // arbiter, or merely observe readiness. In the observation
                // case there can never be another readiness edge, so retire
                // the exact waiter after this one notification.
                if (!retain_terminal[i]) (void)subs[i].close();
            } catch (...) {
                // A failed terminal callback is also one-shot and must not
                // strand an inert waiter.
                (void)subs[i].close();
                if (!first) first = std::current_exception();
            }
        }
        if (first) {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (!state_->notification_failure)
                state_->notification_failure = std::move(first);
        }
        return true;
    }

    std::shared_ptr<State> state_;
};

}  // namespace oreslang::runtime
