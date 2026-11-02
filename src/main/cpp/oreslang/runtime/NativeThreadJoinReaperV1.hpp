#pragma once

#include "oreslang/runtime/NativeThreadV1.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace oreslang::runtime {

// Host-only completion object. The scheduler may poll it or wire a dedicated
// host-notification bridge; observing readiness never calls pthread_join.
// A cancelled/dropped observer does not cancel cleanup owned by the reaper.
class NativeJoinCompletionV1 final {
public:
    bool try_read(ores_thread_status_v1& status, int& value) const noexcept {
        if (!ready_.load(std::memory_order_acquire)) return false;
        status = status_;
        value = value_;
        return true;
    }
private:
    friend class NativeThreadJoinReaperV1;
    void publish(ores_thread_status_v1 status, int value) noexcept {
        value_ = value;
        status_ = status;
        ready_.store(true, std::memory_order_release);
    }
    std::atomic<bool> ready_{false};
    ores_thread_status_v1 status_ = ORES_THREAD_OS_ERROR_V1;
    int value_ = 0;
};

// Bounded host reaper; NEVER instantiated on an AttoLang actor carrier.
// Joining is performed on exactly one dedicated *host* thread. No
// per-waiter OS thread is created. Guest Task wakeups are not yet implemented.
class NativeThreadJoinReaperV1 final {
public:
    explicit NativeThreadJoinReaperV1(std::size_t capacity)
        : capacity_(checked_capacity(capacity)), worker_([this] { run(); }) {
    }

    ~NativeThreadJoinReaperV1() {
        {
            std::lock_guard<std::mutex> guard(lock_);
            closing_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
        // Shutdown waits for actual thread retirement. Must be invoked by
        // a host/supervisor shutdown worker, never a cooperative carrier.
    }

    NativeThreadJoinReaperV1(const NativeThreadJoinReaperV1&) = delete;
    NativeThreadJoinReaperV1& operator=(const NativeThreadJoinReaperV1&) = delete;

    // On success sets handle to NULL, transferring its unique ownership.
    // On backpressure or shutdown, caller retains the same handle and may
    // retry or join during structured cleanup; no silent detach.
    std::shared_ptr<NativeJoinCompletionV1> submit(
            ores_native_thread_v1*& handle) {
        if (!handle) return {};
        auto completion = std::make_shared<NativeJoinCompletionV1>();
        {
            std::lock_guard<std::mutex> guard(lock_);
            if (closing_ || active_ >= capacity_) return {};
            pending_.push_back({handle, completion});
            ++active_;
            handle = nullptr;
        }
        cv_.notify_one();
        return completion;
    }

    std::size_t active() const {
        std::lock_guard<std::mutex> guard(lock_);
        return active_;
    }
private:
    struct Job {
        ores_native_thread_v1* handle;
        std::shared_ptr<NativeJoinCompletionV1> completion;
    };
    static std::size_t checked_capacity(std::size_t n) {
        if (n == 0 || n > 1024)
            throw std::invalid_argument("native_join_reaper_capacity_invalid");
        return n;
    }
    void run() noexcept {
        while (true) {
            Job job{};
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
            auto status = ores_native_thread_join_blocking_v1(job.handle, &result);
            if (status == ORES_THREAD_OK_V1) {
                status = ores_native_thread_release_v1(job.handle);
            } else {
                // An unexpected join failure is a fatal native lifecycle
                // violation. Preserve the raw handle for supervisor/debug
                // inspection rather than detach or silently freeing it.
                // The report carries OS_ERROR, never a success result.
                status = ORES_THREAD_OS_ERROR_V1;
            }
            job.completion->publish(status, result);
            {
                std::lock_guard<std::mutex> guard(lock_);
                --active_;
            }
        }
    }
    const std::size_t capacity_;
    mutable std::mutex lock_;
    std::condition_variable cv_;
    std::deque<Job> pending_;
    std::size_t active_ = 0;
    bool closing_ = false;
    std::thread worker_;
};

} // namespace oreslang::runtime
