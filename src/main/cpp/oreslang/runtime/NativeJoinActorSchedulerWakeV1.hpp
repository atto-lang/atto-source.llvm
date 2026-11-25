#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeThreadJoinSchedulerWakeV1.hpp"

#include <atomic>
#include <memory>

namespace oreslang::runtime {

// Host VM adapter for an ALREADY TRANSFERRED and capacity-reserved wake.
// No guest code is executed by the native pthread join-reaper callback.
// The scheduler holds the resulting wake until a worker can claim its actor
// lane. In particular, this is not yet source-level .atto Task lowering.
//
// Lifetime: the host keeps the scheduler alive until the reaper callbacks
// quiesce and all outstanding wake reservations are either delivered or
// explicitly retired. A C scheduler pointer alone is never a lifetime fence.
class NativeJoinActorSchedulerWakeV1 final
    : public NativeJoinSchedulerWakeQueueV1 {
public:
    static std::shared_ptr<NativeJoinActorSchedulerWakeV1> create(
            OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket) {
        if (!scheduler || !ticket.producer.scheduler_domain ||
                !ticket.producer.generation || !ticket.slot_generation ||
                !ticket.continuation_slot || !ticket.continuation_generation ||
                !ticket.code_generation || !ticket.descriptor_generation)
            return {};
        return std::shared_ptr<NativeJoinActorSchedulerWakeV1>(
            new NativeJoinActorSchedulerWakeV1(scheduler, ticket));
    }

    NativeJoinActorSchedulerWakeV1(
        const NativeJoinActorSchedulerWakeV1&) = delete;
    NativeJoinActorSchedulerWakeV1& operator=(
        const NativeJoinActorSchedulerWakeV1&) = delete;

    ~NativeJoinActorSchedulerWakeV1() override { retire(); }

    // Called by native join-reaper, never a guest scheduler worker.
    // No allocation, no guest callback, and no unbounded waiter thread.
    // The prior RESERVE/TRANSFER step guaranteed a scheduler wake credit.
    void enqueue_reserved_ready() noexcept override {
        unsigned expected = 0;
        if (!state_.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel))
            return;
        published_.store(
            ores_native_actor_scheduler_publish_wake(
                scheduler_, ticket_, ORES_NATIVE_WAKE_READY),
            std::memory_order_release);
    }

    // A cancelled/retired Task must dispose the transferred wake credit,
    // including when its pthread has not yet joined. A reaper callback
    // holding a shared queue reference wins this race and publishes once;
    // Task authority must STILL be checked on the scheduler's wake turn.
    bool retire() noexcept {
        unsigned expected = 0;
        if (!state_.compare_exchange_strong(
                expected, 2, std::memory_order_acq_rel))
            return false;
        return ores_native_actor_scheduler_abort_transferred_wake(
            scheduler_, ticket_) != 0;
    }

    OresNativeWakePublishStatus publish_status() const noexcept {
        return published_.load(std::memory_order_acquire);
    }

private:
    NativeJoinActorSchedulerWakeV1(
            OresNativeActorScheduler* scheduler, OresNativeWakeTicket ticket)
        : scheduler_(scheduler), ticket_(ticket) {}

    OresNativeActorScheduler* const scheduler_;
    const OresNativeWakeTicket ticket_;
    // 0 transferred/unpublished; 1 published/attempted; 2 retired.
    std::atomic<unsigned> state_{0};
    std::atomic<OresNativeWakePublishStatus> published_{ORES_NATIVE_WAKE_INVALID};
};

}  // namespace oreslang::runtime
