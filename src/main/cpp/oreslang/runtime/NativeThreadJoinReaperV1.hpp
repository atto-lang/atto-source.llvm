#pragma once

#include "oreslang/runtime/NativeThreadV1.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Host-owned, read-only completion. Dropping an observer never cancels join.
// The scheduler can later register an event-driven bridge; try_read is not
// permission to poll or block a guest carrier thread.
class NativeJoinCompletionV1 final {
public:
    bool try_read(ores_thread_status_v1& status, int& value) const noexcept {
        if (!ready_.load(std::memory_order_acquire)) return false;
        status = status_;
        value = value_;
        return true;
    }

    // A one-shot scheduler notification; may register before OR after
    // physical join. It is invoked exactly once, without internal locks,
    // either on the reaper host thread or on the registering host thread.
    // CALLBACK MUST ONLY ENQUEUE the logical continuation; it must never
    // directly resume guest code, block a carrier, or throw an exception.
    // Captures should be weak generation-checked task handles, so cancelling
    // a waiter never consumes the underlying thread join right.
    // A second subscriber is rejected, even after completion.
    bool on_ready_once(std::function<void()> notify) {
        if (!notify) return false;
        bool call_now = false;
        {
            std::lock_guard<std::mutex> guard(notify_lock_);
            if (notify_registered_) return false;
            notify_registered_ = true;
            call_now = ready_.load(std::memory_order_acquire);
            if (!call_now) notify_ = std::move(notify);
        }
        if (call_now) invoke_noexcept(notify);
        return true;
    }
private:
    friend class NativeThreadJoinReaperV1;
    static void invoke_noexcept(const std::function<void()>& notify) noexcept {
        try { notify(); } catch (...) {
            // Scheduler notification must not unwind through the native
            // thread-reaper. A failure is a runtime programming error.
            std::terminate();
        }
    }
    void publish(ores_thread_status_v1 status, int value) noexcept {
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> guard(notify_lock_);
            value_ = value;
            status_ = status;
            ready_.store(true, std::memory_order_release);
            notify = std::move(notify_);
        }
        if (notify) invoke_noexcept(notify);
    }
    mutable std::mutex notify_lock_;
    std::function<void()> notify_;
    bool notify_registered_ = false;
    std::atomic<bool> ready_{false};
    ores_thread_status_v1 status_ = ORES_THREAD_OS_ERROR_V1;
    int value_ = 0;
};

// Bounded *host-only* join pool, not a guest Task/Future implementation.
// Multiple workers prevent one stalled pthread from starving unrelated joins.
// The caller must not destroy this pool on a guest scheduler carrier.
//
// Unexpected physical-join failures retain unique handles in failed_ until
// recovery by a supervisor. Destruction with unrecovered failures deliberately
// terminates, rather than silently dropping ownership of native resources.
class NativeThreadJoinReaperV1 final {
public:
    using JoinFunction = ores_thread_status_v1 (*)(ores_native_thread_v1*, int*);

    explicit NativeThreadJoinReaperV1(
            std::size_t capacity, std::size_t workers = 2,
            JoinFunction join = ores_native_thread_join_blocking_v1)
        : capacity_(checked_capacity(capacity)),
          worker_count_(checked_workers(workers, capacity_)),
          join_(checked_join(join)) {
        failed_.reserve(capacity_);
        workers_.reserve(worker_count_);
        try {
            for (std::size_t i = 0; i < worker_count_; ++i)
                workers_.emplace_back([this] { run(); });
        } catch (...) {
            {
                std::lock_guard<std::mutex> guard(lock_);
                closing_ = true;
            }
            cv_.notify_all();
            for (auto& worker : workers_)
                if (worker.joinable()) worker.join();
            throw;
        }
    }

    ~NativeThreadJoinReaperV1() noexcept {
        {
            std::lock_guard<std::mutex> guard(lock_);
            closing_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.get_id() == std::this_thread::get_id())
                std::terminate();
            if (worker.joinable()) worker.join();
        }
        // Never hide failed join rights or pthread handles behind a silent
        // memory leak when the structured supervisor is destroyed.
        if (!failed_.empty()) std::terminate();
    }

    NativeThreadJoinReaperV1(const NativeThreadJoinReaperV1&) = delete;
    NativeThreadJoinReaperV1& operator=(const NativeThreadJoinReaperV1&) = delete;

    // Successful submission consumes *handle exactly once. Queue saturation,
    // allocation failures, or shutdown keep the caller's ownership intact.
    // Throwing allocation failures preserve the handle as well.
    std::shared_ptr<NativeJoinCompletionV1> submit(
            ores_native_thread_v1*& handle) {
        if (!handle) return {};
        auto completion = std::make_shared<NativeJoinCompletionV1>();
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (closing_ || active_ >= capacity_) return {};
            pending_.push_back({handle, completion}); // may throw before move
            ++active_;
            handle = nullptr;
        }
        cv_.notify_one();
        return completion;
    }

    std::size_t active() const noexcept {
        std::lock_guard<std::mutex> guard(lock_);
        return active_;
    }

    std::size_t failed_count() const noexcept {
        std::lock_guard<std::mutex> guard(lock_);
        return failed_.size();
    }

    // Called by an authorized host supervisor after observing an OS failure.
    // The returned handles remain joinable/owned; the supervisor must retry
    // join or escalate without losing them. This is not a guest API.
    std::vector<ores_native_thread_v1*> recover_failed() {
        std::lock_guard<std::mutex> guard(lock_);
        // Allocate a copy BEFORE releasing any ownership. Keep failed_ with
        // its preallocated capacity so run() remains noexcept after recovery.
        auto recovered = failed_;
        // Failed handles continue consuming bounded reaper capacity until the
        // supervisor explicitly takes custody of them.
        if (recovered.size() > active_)
            std::terminate(); // corrupted internal ownership accounting
        active_ -= recovered.size();
        failed_.clear();
        return recovered;
    }

private:
    struct Job {
        ores_native_thread_v1* handle = nullptr;
        std::shared_ptr<NativeJoinCompletionV1> completion;
    };

    static std::size_t checked_capacity(std::size_t n) {
        if (n == 0 || n > 1024)
            throw std::invalid_argument("native_join_reaper_capacity_invalid");
        return n;
    }
    static std::size_t checked_workers(std::size_t n, std::size_t capacity) {
        if (n == 0 || n > 16 || n > capacity)
            throw std::invalid_argument("native_join_reaper_workers_invalid");
        return n;
    }
    static JoinFunction checked_join(JoinFunction fn) {
        if (!fn) throw std::invalid_argument("native_join_reaper_join_null");
        return fn;
    }

    void run() noexcept {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> guard(lock_);
                cv_.wait(guard, [this] { return closing_ || !pending_.empty(); });
                if (pending_.empty()) {
                    if (closing_) return;
                    continue;
                }
                job = std::move(pending_.front());
                pending_.pop_front();
            }

            int result = 0;
            ores_thread_status_v1 status = join_(job.handle, &result);
            bool handle_released = false;
            if (status == ORES_THREAD_OK_V1 ||
                    status == ORES_THREAD_UNEXPECTED_EXIT_V1) {
                // UNEXPECTED_EXIT consumes the physical join right too; it
                // is an error result but NOT a live handle requiring retry.
                const auto release_status =
                    ores_native_thread_release_v1(job.handle);
                if (release_status != ORES_THREAD_OK_V1)
                    status = release_status;
                else
                    handle_released = true;
            }
            {
                std::lock_guard<std::mutex> guard(lock_);
                if (!handle_released) {
                    // Failed ownership remains active and consumes capacity
                    // until recover_failed() transfers it to the supervisor.
                    // Thus failed_.size() can never exceed the reserved bound.
                    failed_.push_back(job.handle);
                } else {
                    --active_;
                }
            }
            job.completion->publish(status, result);
        }
    }

    const std::size_t capacity_;
    const std::size_t worker_count_;
    JoinFunction join_;
    mutable std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Job> pending_;
    std::vector<ores_native_thread_v1*> failed_;
    std::vector<std::thread> workers_;
    std::size_t active_ = 0;
    bool closing_ = false;
};

} // namespace oreslang::runtime
