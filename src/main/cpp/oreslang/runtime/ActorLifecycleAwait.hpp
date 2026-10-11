#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace oreslang::runtime {

/**
 * Pinned native lifecycle-await frame (the narrow Future<void> substrate).
 *
 * The frame lives at one stable shared allocation, even if its owning
 * scheduler handle moves. It owns no actor heap pointers and never performs
 * a blocking Future::get()/wait(). A weak callback from the child ticket
 * records completion; only a parent scheduler CONTROL turn may invoke the
 * guest-resumption callback. There is no tracing garbage collector.
 *
 * This is NOT general source-level await lowering, a borrow-across-suspend
 * proof, or a source-generated state machine. Those need typed MIR and
 * continuation-frame ownership checking before guest grammar is admitted.
 */
class NativeActorLifecycleAwait final
        : public std::enable_shared_from_this<NativeActorLifecycleAwait> {
public:
    struct Result {
        bool success = false;
        std::string error;
    };
    enum class Phase { pending, ready, scheduled, completed, cancelled };
    using Resume = std::function<void(const Result&)>;

    NativeActorLifecycleAwait(const NativeActorLifecycleAwait&) = delete;
    NativeActorLifecycleAwait& operator=(const NativeActorLifecycleAwait&) = delete;

    static std::shared_ptr<NativeActorLifecycleAwait> wait_ready(
            const NativeActorRef& target) {
        auto frame = std::shared_ptr<NativeActorLifecycleAwait>(
            new NativeActorLifecycleAwait());
        const std::weak_ptr<NativeActorLifecycleAwait> weak(frame);
        target.when_ready([weak](const ActorReadyResult& result) {
            if (auto pinned = weak.lock()) pinned->settle(result.ready, result.error);
        });
        return frame;
    }

    static std::shared_ptr<NativeActorLifecycleAwait> wait_done(
            const NativeActorRef& target) {
        auto frame = std::shared_ptr<NativeActorLifecycleAwait>(
            new NativeActorLifecycleAwait());
        const std::weak_ptr<NativeActorLifecycleAwait> weak(frame);
        target.when_done([weak](const ActorDoneResult& result) {
            if (auto pinned = weak.lock()) pinned->settle(result.success, result.error);
        });
        return frame;
    }

    Phase phase() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return phase_;
    }

    /** Cancellation invalidates pending and queued resumption. It never
     * un-registers or dereferences an in-flight child callback. */
    void cancel() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::completed) phase_ = Phase::cancelled;
    }

    /**
     * Parent scheduler tick only. Fails without consuming the terminal
     * observation when parent CONTROL capacity is occupied, so a later tick
     * can retry. The continuation executes strictly on the scheduler's
     * single-flight parent CONTROL lane, not from the child's callback.
     */
    bool try_schedule(NativeActorGroup& parent_group,
                      const NativeActorRef& parent_ref,
                      Resume resume) {
        if (!resume) throw std::invalid_argument("missing lifecycle resume");
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::ready) return false;
        const auto self = shared_from_this();
        if (!parent_group.enqueue_control(
                parent_ref, [self, resume = std::move(resume)] {
                    Result outcome;
                    {
                        std::lock_guard<std::mutex> lock(self->mutex_);
                        if (self->phase_ != Phase::scheduled) return;
                        self->phase_ = Phase::completed;
                        outcome = self->result_;
                    }
                    resume(outcome);
                })) return false;
        phase_ = Phase::scheduled;
        return true;
    }

private:
    NativeActorLifecycleAwait() = default;

    void settle(bool success, const std::string& error) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (phase_ != Phase::pending) return;
        result_ = Result{success, error};
        phase_ = Phase::ready;
    }

    mutable std::mutex mutex_;
    Phase phase_ = Phase::pending;
    Result result_;
};

} // namespace oreslang::runtime
