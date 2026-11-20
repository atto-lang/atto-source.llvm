#pragma once

#include "oreslang/runtime/NativeThreadJoinReaperV1.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <utility>

namespace oreslang::runtime {

// Host-side notification bridge only. The scheduler owns the actual task
// continuation and must supply an enqueue-only callback. This adapter does
// not resume guest code, create an OS waiter thread, or poll for completion.
class NativeJoinTaskWaitV1 final
    : public std::enable_shared_from_this<NativeJoinTaskWaitV1> {
public:
    enum class State : unsigned char { Pending, Notified, Cancelled };
    using Enqueue = std::function<void(std::uint64_t)>;

    static std::shared_ptr<NativeJoinTaskWaitV1> create(
        std::uint64_t generation, Enqueue enqueue) {
        if (generation == 0 || !enqueue) return {};
        return std::shared_ptr<NativeJoinTaskWaitV1>(
            new NativeJoinTaskWaitV1(generation, std::move(enqueue)));
    }

    // Exactly one registration against a physical join completion object.
    // A second registration is rejected by NativeJoinCompletionV1.
    // Use a weak ownership edge to ensure cancelled/retired waiters are not
    // kept alive by an uncompleted native thread. Failed subscription is
    // terminal: an unarmed waiter must never remain pending indefinitely.
    bool attach(const std::shared_ptr<NativeJoinCompletionV1>& completion) {
        if (!completion || attached_.exchange(true, std::memory_order_acq_rel))
            return false;
        auto weak = weak_from_this();
        if (!completion->on_ready_once([weak] {
            if (auto waiter = weak.lock()) waiter->notify();
        })) {
            cancel(); // competing subscriber owns the completion; fail closed
            return false;
        }
        return true;
    }

    // Cancellation and notification race through the same one-way state.
    // A successful cancel prevents future enqueue, without cancelling or
    // consuming the native thread's physical join right. If cancellation
    // loses to notify(), the scheduler must still validate the generation
    // and task cancellation state at dequeue before resuming any guest code.
    bool cancel() noexcept {
        State expected = State::Pending;
        return state_.compare_exchange_strong(
            expected, State::Cancelled, std::memory_order_acq_rel);
    }

    State state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }
    std::uint64_t generation() const noexcept { return generation_; }

private:
    NativeJoinTaskWaitV1(std::uint64_t generation, Enqueue enqueue)
        : generation_(generation), enqueue_(std::move(enqueue)) {}

    void notify() noexcept {
        State expected = State::Pending;
        if (!state_.compare_exchange_strong(
                expected, State::Notified, std::memory_order_acq_rel))
            return;
        // Notified means enqueue ownership was claimed, not that the queue
        // operation has returned. The consumer MUST generation-fence dequeue.
        // Host scheduler callback contract: enqueue only, never execute
        // guest code. Failure to enqueue is fatal rather than silently
        // losing a continuation after claiming notification.
        try { enqueue_(generation_); }
        catch (...) { std::terminate(); }
    }

    const std::uint64_t generation_;
    const Enqueue enqueue_;
    std::atomic<State> state_{State::Pending};
    std::atomic<bool> attached_{false};
};

} // namespace oreslang::runtime
