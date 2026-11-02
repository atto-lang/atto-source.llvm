#pragma once

#include "oreslang/runtime/NativeJoinTaskWaitV1.hpp"
#include "oreslang/runtime/TaskContinuationGate.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace oreslang::runtime {

// Host-only glue from a physical pthread join to an authenticated logical
// Task continuation. Caller owns the C execution/continuation stores and must
// keep them alive through all join callbacks, scheduler claims and cleanup.
//
// A join worker is permitted to publish READY and enqueue an immutable
// generation ticket, but MUST NOT execute a guest frame. The consuming Task
// carrier must call TaskContinuationGate::try_resume(), which re-checks Task
// authority, code/actor identity and READY -> CLAIMED under the store lock.
//
// Construction of the TaskContinuationGate must explicitly require
// ORES_CONTINUATION_BOUNDARY_TASK_JOIN; this adapter cannot mint a gate or
// weaken the boundary or the task's structural cancellation authority.
class NativeJoinTaskContinuationBridgeV1 final {
public:
    using Enqueue = NativeJoinTaskWaitV1::Enqueue;

    static std::shared_ptr<NativeJoinTaskWaitV1> subscribe(
            const std::shared_ptr<NativeJoinCompletionV1>& completion,
            const std::shared_ptr<TaskContinuationGate>& gate,
            std::uint64_t task_generation, Enqueue enqueue) {
        if (!completion || !gate || !enqueue || task_generation == 0)
            return {};
        const std::weak_ptr<TaskContinuationGate> weak = gate;
        auto waiter = NativeJoinTaskWaitV1::create(
            task_generation,
            [weak, enqueue = std::move(enqueue)](std::uint64_t generation) {
                auto active = weak.lock();
                if (active && active->notify_ready()) {
                    // enqueue() never resumes guest code inline. If Task
                    // cancellation wins after READY, the consuming carrier's
                    // generation-fenced try_resume() will reject the ticket.
                    enqueue(generation);
                }
            });
        if (!waiter || !waiter->attach(completion)) return {};
        return waiter;
    }
};

} // namespace oreslang::runtime
