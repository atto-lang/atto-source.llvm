#pragma once

#include "oreslang/runtime/NativeJoinTaskWaitV1.hpp"
#include "oreslang/runtime/TaskContinuationGate.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace oreslang::runtime {

// Host-only bridge from a physical pthread join to a logical Task continuation.
// The reaper's callback ONLY enqueues a generation ticket. It MUST NOT acquire
// Task/continuation-store locks, directly resume a guest frame or block a
// scheduler carrier. The Task scheduler owns ticket dequeue and admission.
//
// The caller constructs TaskContinuationGate with explicit TASK_JOIN boundary
// and retains the C execution/continuation stores until all physical reaper
// callbacks and any claimed continuations have quiesced.
class NativeJoinTaskContinuationBridgeV1 final
    : public std::enable_shared_from_this<NativeJoinTaskContinuationBridgeV1> {
public:
    using Enqueue = NativeJoinTaskWaitV1::Enqueue;

    static std::shared_ptr<NativeJoinTaskContinuationBridgeV1> subscribe(
            const std::shared_ptr<NativeJoinCompletionV1>& completion,
            const std::shared_ptr<TaskContinuationGate>& gate,
            std::uint64_t task_generation, Enqueue enqueue) {
        if (!completion || !gate || !enqueue || task_generation == 0)
            return {};
        auto bridge = std::shared_ptr<NativeJoinTaskContinuationBridgeV1>(
            new NativeJoinTaskContinuationBridgeV1(gate));
        const std::weak_ptr<NativeJoinTaskContinuationBridgeV1> weak = bridge;
        auto waiter = NativeJoinTaskWaitV1::create(
            task_generation,
            [weak, enqueue = std::move(enqueue)](std::uint64_t generation) {
                // Only a bounded, enqueue-only notification is allowed on a
                // native reaper thread. No C store or guest Task operations.
                if (weak.lock()) enqueue(generation);
            });
        if (!waiter) return {};
        // Attach can notify synchronously for an already-completed join.
        // Publish bridge state BEFORE registering that callback, so a
        // concurrent scheduler dequeue never races with waiter_ assignment.
        bridge->waiter_ = std::move(waiter);
        if (!bridge->waiter_->attach(completion)) return {};
        return bridge;
    }

    // Scheduler-dequeue side ONLY: bind immutable generation, one-shot join
    // notification and original Task authority before marking its exact
    // continuation READY. A later TaskCancellation still wins against resume
    // through TaskContinuationGate::try_resume() on the guest carrier.
    bool try_admit_ready(std::uint64_t ticket_generation) noexcept {
        if (!waiter_ ||
                waiter_->generation() != ticket_generation ||
                waiter_->state() != NativeJoinTaskWaitV1::State::Notified)
            return false;
        auto gate = gate_.lock();
        return gate && gate->notify_ready();
    }

    bool cancel_waiter() noexcept {
        return waiter_ && waiter_->cancel();
    }

private:
    explicit NativeJoinTaskContinuationBridgeV1(
            const std::shared_ptr<TaskContinuationGate>& gate)
        : gate_(gate) {}

    std::weak_ptr<TaskContinuationGate> gate_;
    std::shared_ptr<NativeJoinTaskWaitV1> waiter_;
};

} // namespace oreslang::runtime
