#pragma once

// Host CONTROL-plane only: bridge a scheduler actor's generation-specific
// STOP to the request-local contexts registered for that actor incarnation.
//
// IMPORTANT: this is NOT a guest-callable operation lease or a general STOP
// interceptor. Production ingress must route every STOP/retirement for the
// actor through this trusted bridge, or separately cancel all its requests.
// A direct ores_native_actor_scheduler_stop() bypasses this group and remains
// an unproven guest ABI path.
//
// Linearization: seal request registration, close all pinned request stores
// (each close serializes with its pending set/erase under the store mutex),
// then seal the scheduler mailbox, wait for pinned operations to quiesce,
// and finally await actor DONE OUTSIDE carrier threads.
// No guest values/destructors run under this group's registry lock.
// "STOP completed" is never reported until actor DONE is observed.

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace oreslang::runtime {

class RequestLocalStopGroupV1 final {
public:
    RequestLocalStopGroupV1(OresNativeActorScheduler* scheduler,
                            OresNativeActorRef actor) noexcept
        : scheduler_(scheduler), actor_(actor) {}
    RequestLocalStopGroupV1(const RequestLocalStopGroupV1&) = delete;
    RequestLocalStopGroupV1& operator=(const RequestLocalStopGroupV1&) = delete;

    bool register_request(const RequestContextLocalV1::Ptr& request) {
        if (!scheduler_ || !actor_.scheduler_domain || !actor_.generation ||
            !request || !request->identity().valid() ||
            !request->belongs_to(actor_) || !request->active())
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (sealed_) return false;
        for (const auto& weak : registered_)
            if (auto existing = weak.lock())
                if (existing.get() == request.get()) return false;
        registered_.push_back(request);
        return true;
    }

private:
    // Control-only: seal registration and close all pinned request stores.
    // Return strong refs to the caller so they remain alive while STOP is
    // signaled and while any prior in-flight operations drain. Never wait
    // for a task's completion before sending the STOP signal.
    std::optional<std::vector<RequestContextLocalV1::Ptr>>
    seal_and_revoke() {
        std::vector<RequestContextLocalV1::Ptr> pinned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sealed_) return std::nullopt;
            // Allocation may fail: publish a sealed state only after the
            // complete snapshot buffer is ready.
            pinned.reserve(registered_.size());
            sealed_ = true;
            for (const auto& weak : registered_)
                if (auto request = weak.lock())
                    pinned.push_back(std::move(request));
            registered_.clear();
        }
        for (const auto& request : pinned) request->close();
        return pinned;
    }

public:
    // Never call on a scheduler carrier; both pending operation quiescence
    // and wait_done may block until admitted host work has drained.
    // A failed STOP/await is not a DONE receipt.
    bool stop_sealed_and_wait() {
        if (!scheduler_ ||
            ores_native_actor_scheduler_is_any_current_carrier_v1())
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completed_) return false;
        }
        auto pinned = seal_and_revoke();
        if (!pinned) return false;
        // After all stores are closed, STOP may be published immediately.
        // Do not await pinned copies before issuing this scheduler signal.
        if (!ores_native_actor_scheduler_stop_sealed(scheduler_, actor_))
            return false;
        // A pre-STOP copy/mutation may retain a pinned cell. The CONTROL
        // caller alone waits for these leases and the actor DONE barrier;
        // no guest destructor runs under the registry/store/graph mutex.
        for (const auto& request : *pinned)
            if (!request->wait_closed_quiescent()) return false;
        if (!ores_native_actor_scheduler_wait_done(scheduler_, actor_))
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        completed_ = true;
        return true;
    }

    // A trusted external STOP/retire path may have signaled the scheduler
    // before the request-owner CONTROL plane was notified. This explicit
    // reconciliation closes its stores and joins pinned operations; it never
    // executes on a carrier, and it never emits a second STOP.
    // It is NOT automatic cleanup for arbitrary raw scheduler callers.
    bool finish_after_external_stop() {
        if (!scheduler_ ||
            ores_native_actor_scheduler_is_any_current_carrier_v1() ||
            !ores_native_actor_scheduler_is_stopping_v1(scheduler_, actor_))
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completed_) return false;
        }
        auto pinned = seal_and_revoke();
        if (!pinned) return false;
        for (const auto& request : *pinned)
            if (!request->wait_closed_quiescent()) return false;
        if (!ores_native_actor_scheduler_wait_done(scheduler_, actor_))
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        completed_ = true;
        return true;
    }

    bool sealed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sealed_;
    }

    bool completed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_;
    }

private:
    OresNativeActorScheduler* const scheduler_;
    const OresNativeActorRef actor_;
    mutable std::mutex mutex_;
    bool sealed_ = false;
    bool completed_ = false;
    std::vector<std::weak_ptr<RequestContextLocalV1>> registered_;
};

} // namespace oreslang::runtime
