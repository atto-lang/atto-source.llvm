#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * One bounded, host-owned supervisor CONTROL executor PER RUNTIME.
 * Never instantiate per ActorGroup, actor, message, or supervisor callback.
 *
 * A runtime retaining this shared scheduler can admit supervisor turns even
 * while all user actor carriers in an unrelated pool are occupied. This is
 * infrastructure, not a guarantee against a policy blocking every CONTROL
 * carrier: guest policies must suspend cooperatively, not block a carrier.
 *
 * Adapter uses weak ownership so a late actor finalizer cannot dereference a
 * destroyed scheduler; it instead fails admission and increments the lane's
 * undelivered count.
 */
class SupervisorControlScheduler final
    : public std::enable_shared_from_this<SupervisorControlScheduler> {
public:
    using Task = std::function<void()>;
    using Enqueue = std::function<bool(Task)>;

    SupervisorControlScheduler(std::size_t threads, std::size_t capacity)
        : capacity_(capacity) {
        if (threads == 0 || capacity == 0 || threads > 64)
            throw std::invalid_argument("invalid supervisor CONTROL scheduler limits");
        workers_.reserve(threads);
        try {
            for (std::size_t i = 0; i < threads; ++i)
                workers_.emplace_back([this] { worker_loop(); });
        } catch (...) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                accepting_ = false;
            }
            wakeup_.notify_all();
            for (auto& worker : workers_)
                if (worker.joinable()) worker.join();
            throw;
        }
    }

    SupervisorControlScheduler(const SupervisorControlScheduler&) = delete;
    SupervisorControlScheduler& operator=(const SupervisorControlScheduler&) = delete;

    Enqueue adapter() {
        std::weak_ptr<SupervisorControlScheduler> weak = shared_from_this();
        return [weak](Task task) {
            auto scheduler = weak.lock();
            return scheduler && scheduler->enqueue(std::move(task));
        };
    }

    bool enqueue(Task task) {
        if (!task) return false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!accepting_ || queue_.size() >= capacity_) return false;
            queue_.push_back(std::move(task));
        }
        wakeup_.notify_one();
        return true;
    }

    std::size_t queued() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.size();
    }

    /** Gracefully drain committed CONTROL work; no new admission afterward. */
    void close() {
        // Shutdown requests may race. Only one caller may touch std::thread
        // join() handles; simultaneous joins on the same thread are UB.
        std::lock_guard<std::mutex> close_guard(close_mutex_);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            accepting_ = false;
        }
        wakeup_.notify_all();
        for (auto& worker : workers_) {
            if (!worker.joinable()) continue;
            if (worker.get_id() == std::this_thread::get_id())
                throw std::logic_error("CONTROL scheduler cannot join its own carrier");
            worker.join();
        }
    }

    ~SupervisorControlScheduler() {
        // Ownership must outlive admitted guest CONTROL jobs. close() is
        // called by the runtime's host lifecycle, not by a guest callback.
        {
            std::lock_guard<std::mutex> guard(mutex_);
            accepting_ = false;
        }
        wakeup_.notify_all();
        for (auto& worker : workers_)
            if (worker.joinable()) {
                if (worker.get_id() == std::this_thread::get_id())
                    std::terminate();
                worker.join();
            }
    }

private:
    void worker_loop() noexcept {
        for (;;) {
            Task work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wakeup_.wait(lock, [this] {
                    return !queue_.empty() || !accepting_;
                });
                if (queue_.empty() && !accepting_) return;
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            try { work(); }
            catch (...) {
                // The supervisor's own lane catches and accounts policy
                // errors. No failed CONTROL turn may escape and kill a
                // runtime-wide carrier.
            }
        }
    }

    const std::size_t capacity_;
    std::mutex close_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::deque<Task> queue_;
    bool accepting_ = true;
    std::vector<std::thread> workers_;
};

} // namespace oreslang::runtime
