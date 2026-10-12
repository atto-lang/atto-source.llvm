#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/*
 * Bounded actor / supervisor logical-lane dispatcher.
 *
 * No carrier is blocked waiting for actor admission. Callers only enqueue;
 * exactly one accepted handler is physically scheduled at a time. The
 * executor posts to an independently managed carrier pool; it NEVER invokes
 * guest code on a source-message, timer, loan, or child-failure producer.
 *
 * Parent/grandaddy supervisors must have their OWN instance. CONTROL has
 * reserved capacity and takes priority over the bounded DATA mailbox.
 *
 * Executor contract: submit() must accept a scheduled task without throwing
 * and must eventually execute that task (or terminate the host). If that
 * guarantee cannot be met, the caller MUST NOT accept actor user work.
 * A late source callback cannot hold a raw pointer to this object: retain
 * a shared_ptr until it has completed posting its event.
 */
class ActorExclusiveScheduler final
    : public std::enable_shared_from_this<ActorExclusiveScheduler> {
public:
    using Task = std::function<void()>;
    using Executor = std::function<void(Task)>;
    using Failure = std::function<void()>;

    ActorExclusiveScheduler(std::size_t data_capacity, Executor executor,
                            Failure on_handler_failure = {})
        : data_capacity_(data_capacity), executor_(std::move(executor)),
          on_handler_failure_(std::move(on_handler_failure)) {
        if (data_capacity == 0 || !executor_)
            throw std::invalid_argument("invalid native actor lane capacity or executor");
    }

    ActorExclusiveScheduler(const ActorExclusiveScheduler&) = delete;
    ActorExclusiveScheduler& operator=(const ActorExclusiveScheduler&) = delete;

    bool post_data(Task handler, Task on_cancel = {}) {
        if (!handler) throw std::invalid_argument("empty actor data handler");
        Task dispatch;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closing_ || data_.size() >= data_capacity_) return false;
            data_.push_back({std::move(handler), std::move(on_cancel)});
            dispatch = pop_if_idle_locked();
        }
        schedule(std::move(dispatch));
        return true;
    }

    // Reserve one CONTROL slot even when DATA is full. Coalescing multiple
    // wakeups is safe because CONTROL handlers drain state until quiescent;
    // the single merged handler is provided by the owner, not the child.
    bool post_control(Task handler) {
        if (!handler) throw std::invalid_argument("empty actor CONTROL handler");
        Task dispatch;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (finalized_ || control_pending_) return false;
            control_pending_ = true;
            control_ = std::move(handler);
            dispatch = pop_if_idle_locked();
        }
        schedule(std::move(dispatch));
        return true;
    }

    // Stop accepting DATA but never revoke already accepted CONTROL.
    // Ordinary DATA pending on shutdown is abandoned; cleanup is invoked
    // through the reserved parent CONTROL channel separately.
    void close() {
        std::deque<Data> cancelled;
        Task dispatch;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            closing_ = true;
            cancelled.swap(data_);
            dispatch = pop_if_idle_locked();
        }
        // Tell each already-accepted queued call that it was cancelled:
        // no future can remain unresolved just because close drained DATA.
        // Never run a callback while holding the mailbox lock.
        for (auto& item : cancelled) {
            if (item.on_cancel) {
                try { item.on_cancel(); }
                catch (...) { /* cancellation hooks cannot block siblings */ }
            }
        }
        schedule(std::move(dispatch));
    }

    // Can only be called after lifecycle done physically quiesces. It
    // prevents a stale callback from resurrecting retired scheduler state.
    bool finalize() {
        std::lock_guard<std::mutex> guard(mutex_);
        if (in_flight_ || control_pending_ || !data_.empty())
            return false;
        closing_ = true;
        finalized_ = true;
        return true;
    }

    std::size_t pending_data() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return data_.size();
    }

private:
    Task pop_if_idle_locked() {
        if (in_flight_) return {};
        Task selected;
        if (control_pending_) {
            selected = std::move(control_);
            control_pending_ = false;
        } else if (!closing_ && !data_.empty()) {
            selected = std::move(data_.front().handler);
            data_.pop_front();
        }
        if (selected) in_flight_ = true;
        return selected;
    }

    void schedule(Task selected) {
        if (!selected) return;
        auto keep_alive = shared_from_this();
        executor_([keep_alive, selected = std::move(selected)]() mutable {
            try {
                selected();
            } catch (...) {
                if (keep_alive->on_handler_failure_) {
                    try { keep_alive->on_handler_failure_(); }
                    catch (...) { /* failure callback is enqueue-only */ }
                }
            }
            keep_alive->turn_completed();
        });
    }

    void turn_completed() {
        Task dispatch;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!in_flight_) std::terminate();
            in_flight_ = false;
            dispatch = pop_if_idle_locked();
        }
        schedule(std::move(dispatch));
    }

    const std::size_t data_capacity_;
    const Executor executor_;
    const Failure on_handler_failure_;
    mutable std::mutex mutex_;
    bool in_flight_ = false;
    bool closing_ = false;
    bool finalized_ = false;
    bool control_pending_ = false;
    struct Data {
        Task handler;
        Task on_cancel;
    };
    Task control_;
    std::deque<Data> data_;
};

} // namespace oreslang::runtime
