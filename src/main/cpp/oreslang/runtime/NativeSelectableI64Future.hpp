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

/** Source-independent, bounded Future<i64> readiness for native static select.
 * Readiness callbacks are advisory and run outside the future mutex.
 * Only a select conditional-commit callback may consume a ready registration.
 */
template <std::size_t MaxWaiters = 64>
class NativeSelectableI64Future final {
    static_assert(MaxWaiters > 0, "positive future waiter capacity required");
public:
    enum class Status { Pending, Resolved, Rejected, Cancelled };
    struct Receipt final {
        Status status = Status::Pending;
        std::int64_t value = 0;
        std::exception_ptr error;
    };
private:
    struct Waiter final {
        std::uint64_t id = 0;
        bool active = false;
        std::function<void()> callback;
    };
    struct State final {
        mutable std::mutex mutex;
        std::array<Waiter, MaxWaiters> waiters{};
        std::uint64_t next_id = 1;
        Status status = Status::Pending;
        std::int64_t value = 0;
        std::exception_ptr error;
        std::exception_ptr notification_failure;
    };
public:
    class Subscription final {
        friend class NativeSelectableI64Future;
        std::weak_ptr<State> state_;
        std::size_t index_ = MaxWaiters;
        std::uint64_t id_ = 0;
        Subscription(const std::shared_ptr<State>& state,
                     std::size_t index, std::uint64_t id)
            : state_(state), index_(index), id_(id) {}
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
            waiter.callback = {};
            return true;
        }
        bool active() const {
            auto state = state_.lock();
            if (!state) return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            return index_ < MaxWaiters &&
                state->waiters[index_].active &&
                state->waiters[index_].id == id_;
        }
    };

    NativeSelectableI64Future() : state_(std::make_shared<State>()) {}

    template <class Callback>
    Subscription subscribe_ready(Callback&& callback) {
        std::function<void(const Subscription&)> consumer(
            std::forward<Callback>(callback));
        if (!consumer) throw std::invalid_argument("empty future callback");
        Subscription token;
        std::function<void()> immediate;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            std::size_t index = MaxWaiters;
            for (std::size_t i=0;i<MaxWaiters;++i) {
                if (!state_->waiters[i].active) { index=i; break; }
            }
            if (index == MaxWaiters)
                throw std::overflow_error("future readiness capacity exhausted");
            if (state_->next_id == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("future waiter identity exhausted");
            const auto id = state_->next_id;
            token = Subscription(state_,index,id);
            std::function<void()> wrapped([consumer=std::move(consumer),token]() mutable {
                consumer(token);
            });
            if (state_->status != Status::Pending) immediate=wrapped;
            auto& waiter=state_->waiters[index];
            waiter.id=id;
            waiter.callback=std::move(wrapped);
            waiter.active=true;
            ++state_->next_id;
        }
        if (immediate) {
            try { immediate(); }
            catch (...) { (void)token.close(); throw; }
        }
        return token;
    }

    bool try_commit_ready(const Subscription& token, Receipt& receipt) {
        receipt = {};
        auto owner=token.state_.lock();
        if (!owner || owner.get()!=state_.get() || token.index_>=MaxWaiters)
            return false;
        std::lock_guard<std::mutex> guard(state_->mutex);
        auto& waiter=state_->waiters[token.index_];
        if (!waiter.active || waiter.id!=token.id_ ||
            state_->status==Status::Pending) return false;
        receipt.status=state_->status;
        receipt.value=state_->value;
        receipt.error=state_->error;
        waiter.active=false;
        waiter.callback={};
        return true;
    }

    bool resolve(std::int64_t value) {
        return settle(Status::Resolved,value,{});
    }
    bool reject(std::exception_ptr error) {
        if (!error) throw std::invalid_argument("future rejection requires an error");
        return settle(Status::Rejected,0,std::move(error));
    }
    bool cancel() { return settle(Status::Cancelled,0,{}); }

    Status status() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->status;
    }
    std::size_t pending_waiters() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        std::size_t count=0;
        for (const auto& waiter: state_->waiters)
            if (waiter.active) ++count;
        return count;
    }
    std::exception_ptr notification_failure() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return state_->notification_failure;
    }

private:
    bool settle(Status next, std::int64_t value, std::exception_ptr error) {
        std::array<std::function<void()>,MaxWaiters> callbacks{};
        std::size_t count=0;
        {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if (state_->status!=Status::Pending) return false;
            // Capture before terminal publication; copying callbacks can
            // throw without changing the future state.
            for(const auto& waiter:state_->waiters)
                if(waiter.active)callbacks[count++]=waiter.callback;
            state_->status=next;
            state_->value=value;
            state_->error=std::move(error);
        }
        std::exception_ptr failure;
        for(std::size_t i=0;i<count;++i) {
            try { if(callbacks[i])callbacks[i](); }
            catch (...) { if(!failure)failure=std::current_exception(); }
        }
        if (failure) {
            std::lock_guard<std::mutex> guard(state_->mutex);
            if(!state_->notification_failure)
                state_->notification_failure=std::move(failure);
        }
        return true;
    }
    std::shared_ptr<State> state_;
};

/** Retirement closes admission immediately. Physical retirement is blocked
 * until all executing synchronous continuation leases have returned.
 * Must recheck the token under the select conditional-commit hook and prior
 * to continuation dispatch. A new actor uses a new fence.
 */
class NativeActorGenerationFence final {
    struct State final {
        mutable std::mutex mutex;
        std::uint64_t generation=1;
        bool live=true;
        std::size_t executing=0;
    };
public:
    class Token final {
        friend class NativeActorGenerationFence;
        std::weak_ptr<State> state_;
        std::uint64_t epoch_=0;
        Token(std::shared_ptr<State> state,std::uint64_t epoch)
            :state_(std::move(state)),epoch_(epoch){}
    public:
        Token()=default;
        bool current() const {
            auto state=state_.lock();
            if(!state)return false;
            std::lock_guard<std::mutex> guard(state->mutex);
            return state->live && state->generation==epoch_;
        }
        template<class Callback>
        bool run_if_current(Callback&& callback) const {
            auto state=state_.lock();
            if(!state)return false;
            {
                std::lock_guard<std::mutex> guard(state->mutex);
                if(!state->live || state->generation!=epoch_)return false;
                ++state->executing;
            }
            struct Release final {
                std::shared_ptr<State> state;
                ~Release() {
                    std::lock_guard<std::mutex> guard(state->mutex);
                    --state->executing;
                }
            } release{state};
            std::forward<Callback>(callback)();
            return true;
        }
    };

    NativeActorGenerationFence():state_(std::make_shared<State>()){}
    Token capture() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return Token(state_,state_->generation);
    }
    bool retire() {
        std::lock_guard<std::mutex> guard(state_->mutex);
        if(!state_->live)return false;
        state_->live=false;
        ++state_->generation;
        return true;
    }
    bool physically_retired() const {
        std::lock_guard<std::mutex> guard(state_->mutex);
        return !state_->live && state_->executing==0;
    }
private:
    std::shared_ptr<State> state_;
};

} // namespace oreslang::runtime
