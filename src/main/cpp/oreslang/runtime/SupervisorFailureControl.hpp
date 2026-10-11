#pragma once

#include "oreslang/runtime/ExceptionPolicy.hpp"
#include "oreslang/runtime/SupervisorEnqueueGuard.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::runtime {

/** Immutable value-only terminal notice; never an exception_ptr or heap loan. */
struct SupervisorFailureNotice {
    std::uint64_t supervisor_id;
    std::uint64_t runtime_id;
    std::uint64_t actor_id;
    std::uint64_t generation;
    std::optional<std::uint64_t> spawned_by;
    std::string diagnostic;
};

/**
 * Host-only, bounded supervisor CONTROL delivery. The runtime must supply an
 * enqueue-only scheduler adapter; the child finalizer only publishes a value.
 * No actor guest code is executed directly by publish().
 *
 * The lane is retained by actor lifecycle tickets after group close. Pending
 * notices are NOT silently treated as delivered when dispatch fails.
 */
class SupervisorFailureControl final : public std::enable_shared_from_this<SupervisorFailureControl> {
public:
    using Handler = std::function<void(const SupervisorFailureNotice&)>;
    using Enqueue = std::function<bool(std::function<void()>)>;

    explicit SupervisorFailureControl(std::uint64_t supervisor_id,
                                      std::size_t capacity = 128)
        : supervisor_id_(supervisor_id), capacity_(capacity) {
        if (!supervisor_id || !capacity)
            throw std::invalid_argument("invalid supervisor CONTROL admission");
    }

    void install(Handler handler, Enqueue enqueue) {
        if (!handler || !enqueue) throw std::invalid_argument("missing supervisor callback");
        std::lock_guard<std::mutex> guard(mutex_);
        if (handler_) throw std::logic_error("supervisor CONTROL handler already installed");
        handler_ = std::move(handler);
        enqueue_ = std::move(enqueue);
        schedule_locked();
    }

    void publish(SupervisorFailureNotice notice) noexcept {
        try {
            notice.diagnostic = bounded_control_diagnostic(notice.diagnostic);
            std::lock_guard<std::mutex> guard(mutex_);
            if (notice.supervisor_id != supervisor_id_ || !notice.actor_id) {
                ++undelivered_;
                return;
            }
            if (failed_ || queue_.size() >= capacity_) {
                ++undelivered_;
                return;
            }
            queue_.push_back(std::move(notice));
            schedule_locked();
        } catch (...) {
            std::lock_guard<std::mutex> guard(mutex_);
            ++undelivered_;
            failed_ = true;
        }
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.size();
    }
    std::size_t undelivered() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return undelivered_;
    }
    bool healthy() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return !failed_ && undelivered_ == 0;
    }

private:
    void schedule_locked() {
        if (!handler_ || scheduled_ || failed_ || queue_.empty()) return;
        scheduled_ = true;
        try {
            auto self = shared_from_this();
            if (!SupervisorEnqueueGuard::submit(enqueue_, [self] { self->drain(); })) {
                scheduled_ = false;
                failed_ = true;
                undelivered_ += queue_.size();
            }
        } catch (...) {
            scheduled_ = false;
            failed_ = true;
            undelivered_ += queue_.size();
        }
    }

    void drain() noexcept {
        std::size_t count = 0;
        while (count < 32) {
            SupervisorFailureNotice next;
            Handler handler;
            try {
                std::lock_guard<std::mutex> guard(mutex_);
                if (failed_ || queue_.empty()) {
                    scheduled_ = false;
                    return;
                }
                next = queue_.front();
                handler = handler_;
            } catch (...) {
                std::lock_guard<std::mutex> guard(mutex_);
                failed_ = true;
                undelivered_ += queue_.size();
                scheduled_ = false;
                return;
            }
            try { handler(next); }
            catch (...) {
                std::lock_guard<std::mutex> guard(mutex_);
                failed_ = true;
                undelivered_ += queue_.size();
                scheduled_ = false;
                return;
            }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                queue_.pop_front();
            }
            ++count;
        }
        std::lock_guard<std::mutex> guard(mutex_);
        scheduled_ = false;
        schedule_locked();
    }

    const std::uint64_t supervisor_id_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<SupervisorFailureNotice> queue_;
    Handler handler_;
    Enqueue enqueue_;
    bool scheduled_ = false;
    bool failed_ = false;
    std::size_t undelivered_ = 0;
};

} // namespace oreslang::runtime
