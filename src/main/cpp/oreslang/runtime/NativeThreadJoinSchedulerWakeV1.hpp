#pragma once

#include "oreslang/runtime/NativeThreadJoinReaperV1.hpp"

#include <memory>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/*
 * A scheduler-provided, already-reserved wake slot. The queue MUST keep the
 * reservation until the completion is delivered or its Task is retired.
 * Publishing a wake must never run guest code, park a carrier or throw.
 * A queued stale ticket is checked again by TaskContinuationGate::try_resume.
 */
class NativeJoinSchedulerWakeQueueV1 {
public:
    virtual ~NativeJoinSchedulerWakeQueueV1() = default;
    virtual void enqueue_reserved_ready() noexcept = 0;
};

/*
 * Bind physical join completion to a *previously committed* suspended Task
 * continuation (TaskContinuationGate in the LLVM scheduler). The completion
 * reaper does not own either the Task gate or scheduler queue. A Task cancel /
 * generation retirement invalidates the gate, and weak ownership prevents
 * callbacks from resurrecting retired actor/task/scheduler state.
 *
 * This is an event notification bridge, not yet guest Thread.join_async()
 * lowering. Physical pthread_join stays on the bounded host reaper.
 *
 * Gate::notify_ready() must be noexcept and must atomically reject cancelled,
 * retired or mismatched continuations. Queue publication must only enqueue a
 * pre-reserved wake ticket; guest execution uses the separate try_resume gate.
 */
class NativeThreadJoinSchedulerWakeV1 final {
public:
    template <typename Gate>
    static bool subscribe(
            const std::shared_ptr<NativeJoinCompletionV1>& completion,
            std::weak_ptr<Gate> gate,
            std::weak_ptr<NativeJoinSchedulerWakeQueueV1> queue) {
        // This callback may execute on a native join-reaper thread. Guest
        // exceptions must never unwind through that boundary: reject a
        // potentially throwing or non-boolean gate during compilation.
        static_assert(
            std::is_same_v<decltype(std::declval<Gate&>().notify_ready()), bool> &&
                noexcept(std::declval<Gate&>().notify_ready()),
            "native join wake gate notify_ready must return bool and be noexcept");
        if (!completion || gate.expired() || queue.expired()) return false;
        return completion->on_ready_once(
            [gate = std::move(gate), queue = std::move(queue)]() noexcept {
                auto live_gate = gate.lock();
                if (!live_gate) return;
                auto live_queue = queue.lock();
                if (!live_queue) return;
                // notify_ready() performs generation/cancellation admission.
                // Enqueueing may race a later cancellation; try_resume() must
                // re-check authority before entering any guest continuation.
                if (live_gate->notify_ready())
                    live_queue->enqueue_reserved_ready();
            });
    }
};

}  // namespace oreslang::runtime
