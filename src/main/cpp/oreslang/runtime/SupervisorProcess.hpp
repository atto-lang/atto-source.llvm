#pragma once

#include "oreslang/runtime/SupervisorFailureControl.hpp"
#include "oreslang/runtime/SupervisorEnqueueGuard.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::runtime {

/**
 * Host-side logical supervisor process with one exclusive CONTROL lane.
 * Independent supervisors may execute concurrently via the shared runtime
 * scheduler. No guest source-level on_panic implementation is implied.
 */
class SupervisorProcess final : public std::enable_shared_from_this<SupervisorProcess> {
public:
    enum class Directive { restart, stop, escalate, acknowledge };
    static constexpr bool valid_directive(Directive value) noexcept {
        switch (value) {
            case Directive::restart:
            case Directive::stop:
            case Directive::escalate:
            case Directive::acknowledge: return true;
        }
        return false;
    }
    struct Resolution {
        SupervisorFailureNotice notice;
        Directive requested;
    };
    // Nullopt means logical await: the carrier returns while the lease stays
    // owned. A future resolver calls resume() to schedule an admitted turn.
    using Policy = std::function<std::optional<Directive>(
        const SupervisorFailureNotice&)>;
    using DecisionSink = std::function<void(const Resolution&)>;
    using Enqueue = SupervisorFailureControl::Enqueue;

    SupervisorProcess(std::uint64_t supervisor_id, std::uint64_t runtime_id,
                      std::size_t capacity, Enqueue enqueue,
                      Policy on_failure, DecisionSink sink)
        : supervisor_id_(supervisor_id), runtime_id_(runtime_id),
          capacity_(capacity), enqueue_(std::move(enqueue)),
          policy_(std::move(on_failure)), sink_(std::move(sink)) {
        if (!supervisor_id || !runtime_id || !capacity
            || !enqueue_ || !policy_ || !sink_)
            throw std::invalid_argument("invalid supervisor process admission");
    }

    std::uint64_t supervisor_id() const noexcept { return supervisor_id_; }
    std::uint64_t runtime_id() const noexcept { return runtime_id_; }

    /** Trusted CONTROL receiver; does not run a user callback inline. */
    void accept(const SupervisorFailureNotice& notice) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (notice.supervisor_id != supervisor_id_
            || notice.runtime_id != runtime_id_) {
            throw std::invalid_argument("foreign supervisor process notice");
        }
        if (failed_ || closed_ || close_after_commit_ || mailbox_.size() >= capacity_) {
            ++undelivered_;
            throw std::logic_error("supervisor process cannot admit CONTROL");
        }
        mailbox_.push_back(notice);
        schedule_locked();
    }

    /**
     * Completes one suspended policy. Can be called from any completion
     * thread, but resolution and decision sink execute only in the scheduled
     * continuation. A second/stale-epoch resume is rejected until the first
     * is admitted. A caller must wait for suspension before calling resume.
     */
    bool resume(std::uint64_t expected_epoch, Directive decision) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        // Every new failure callback owns a fresh continuation generation.
        // Without this fence a late completion from a previous callback
        // could resume the next child's failure with a stale decision.
        if (!expected_epoch || expected_epoch != epoch_
            || !suspended_ || resume_enqueued_ || failed_ || closed_)
            return false;
        if (!valid_directive(decision)) {
            // Native callers can manufacture unknown enum values through
            // FFI or an explicit cast. Never send one to a lifecycle owner.
            fail_locked();
            return false;
        }
        resume_enqueued_ = true;
        try {
            auto self = shared_from_this();
            if (!SupervisorEnqueueGuard::submit(
                    enqueue_, [self, expected_epoch, decision] {
                        self->finish(expected_epoch, decision);
                    })) {
                fail_locked();
                return false;
            }
        } catch (...) {
            fail_locked();
            return false;
        }
        return true;
    }

    /** Capture during the current callback; pass unchanged to resume(). */
    std::uint64_t current_epoch() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return active_ && !failed_ && !closed_ ? epoch_ : 0;
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return mailbox_.size();
    }
    std::size_t resolved() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return resolved_;
    }
    std::size_t undelivered() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return undelivered_;
    }
    bool callback_active() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return active_;
    }
    bool healthy() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return !failed_ && !closed_ && !close_after_commit_ && undelivered_ == 0;
    }
    void close() {
        // Linearize shutdown with actual lifecycle decision side effects.
        // A recursive fence supports a trusted decision that stops its own
        // supervisor without deadlocking the CONTROL carrier.
        std::lock_guard<std::recursive_mutex> decision_guard(decision_fence_);
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return;
        if (committing_decision_ && !failed_) {
            close_after_commit_ = true;
            return;
        }
        closed_ = true;
        if (!failed_) undelivered_ += mailbox_.size();
        mailbox_.clear();
        active_ = false;
        policy_started_ = false;
        suspended_ = false;
        resume_enqueued_ = false;
    }

private:
    void schedule_locked() {
        if (active_ || failed_ || closed_ || mailbox_.empty()) return;
        if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
            fail_locked();
            return;
        }
        const std::uint64_t generation = ++epoch_;
        active_ = true;
        policy_started_ = false;
        try {
            auto self = shared_from_this();
            if (!SupervisorEnqueueGuard::submit(
                    enqueue_, [self, generation] { self->begin(generation); })) fail_locked();
        } catch (...) {
            fail_locked();
        }
    }

    void begin(std::uint64_t generation) noexcept {
        SupervisorFailureNotice next;
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            // A duplicated or delayed scheduler callback cannot invoke policy
            // twice, overlap the current policy, or enter a newer turn.
            if (failed_ || closed_ || !active_ || policy_started_
                || generation != epoch_ || mailbox_.empty()) return;
            policy_started_ = true;
            next = mailbox_.front();
        } catch (...) {
            std::lock_guard<std::mutex> guard(mutex_);
            fail_locked();
            return;
        }
        std::optional<Directive> result;
        try { result = policy_(next); }
        catch (...) {
            std::lock_guard<std::mutex> guard(mutex_);
            fail_locked();
            return;
        }
        if (!result) {
            std::lock_guard<std::mutex> guard(mutex_);
            if (!failed_ && !closed_) suspended_ = true;
            return;
        }
        finish(generation, *result);
    }

    void finish(std::uint64_t generation, Directive decision) noexcept {
        std::lock_guard<std::recursive_mutex> decision_guard(decision_fence_);
        Resolution resolution;
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            // Validate again on the CONTROL carrier: checking the epoch at
            // resume() admission alone does not fence a stale queued callback.
            if (failed_ || closed_ || !active_ || generation != epoch_
                || !policy_started_ || mailbox_.empty()) return;
            if (!valid_directive(decision)) {
                fail_locked();
                return;
            }
            resolution = Resolution{mailbox_.front(), decision};
            suspended_ = false;
        } catch (...) {
            std::lock_guard<std::mutex> guard(mutex_);
            fail_locked();
            return;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            committing_decision_ = true;
        }
        try {
            // A host effect may close its owning supervisor reentrantly.
            // Defer closure until this successfully applied decision commits.
            sink_(resolution);
        } catch (...) {
            std::lock_guard<std::mutex> guard(mutex_);
            committing_decision_ = false;
            // The host side effect may have occurred before the exception.
            // Never replay it or count it as committed. Complete any
            // self-requested close after accounting for the failed decision.
            fail_locked();
            if (close_after_commit_) {
                closed_ = true;
                mailbox_.clear();
                active_ = false;
                policy_started_ = false;
                suspended_ = false;
                resume_enqueued_ = false;
                close_after_commit_ = false;
            }
            return;
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            committing_decision_ = false;
            if (failed_ || closed_) return;
            mailbox_.pop_front();
            ++resolved_;
            active_ = false;
            policy_started_ = false;
            resume_enqueued_ = false;
            if (close_after_commit_) {
                closed_ = true;
                undelivered_ += mailbox_.size();
                mailbox_.clear();
            } else {
                schedule_locked();
            }
        }
    }

    void fail_locked() noexcept {
        if (failed_ || closed_) return;
        failed_ = true;
        undelivered_ += mailbox_.size();
        mailbox_.clear(); // Failed CONTROL never retains owned notice data.
        active_ = false;
        policy_started_ = false;
        suspended_ = false;
        resume_enqueued_ = false;
    }

    const std::uint64_t supervisor_id_;
    const std::uint64_t runtime_id_;
    const std::size_t capacity_;
    Enqueue enqueue_;
    Policy policy_;
    DecisionSink sink_;
    // Always acquire decision_fence_ before mutex_ for close/finish.
    mutable std::recursive_mutex decision_fence_;
    mutable std::mutex mutex_;
    std::deque<SupervisorFailureNotice> mailbox_;
    bool active_ = false;
    bool policy_started_ = false;
    bool suspended_ = false;
    bool resume_enqueued_ = false;
    bool committing_decision_ = false;
    bool close_after_commit_ = false;
    bool failed_ = false;
    bool closed_ = false;
    std::uint64_t epoch_ = 0;
    std::size_t resolved_ = 0;
    std::size_t undelivered_ = 0;
};

} // namespace oreslang::runtime
