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
// then seal the scheduler mailbox and await quiescence OUTSIDE carrier threads.
// No guest values/destructors run under this group's registry lock.
// "STOP completed" is never reported until actor DONE is observed.

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
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

    // Nonblocking admission fence: no new requests register afterwards.
    // Close each current scope outside the registry mutex; closing cannot
    // create a new value after it acquires that scope's store mutex.
    bool seal_and_revoke() {
        std::vector<RequestContextLocalV1::Ptr> pinned;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (sealed_) return false;
            sealed_ = true;
            pinned.reserve(registered_.size());
            for (const auto& weak : registered_)
                if (auto request = weak.lock())
                    pinned.push_back(std::move(request));
            registered_.clear();
        }
        for (const auto& request : pinned) request->close();
        return true;
    }

    // Never call on a scheduler carrier; wait_done may block while the actor
    // completes its current turn. A failed STOP/await is not a DONE receipt.
    // The bridge's destructor does not stop the actor implicitly.
    bool stop_sealed_and_wait() {
        if (!scheduler_ ||
            ores_native_actor_scheduler_is_current_carrier(scheduler_))
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completed_) return false;
        }
        if (!seal_and_revoke()) return false;
        if (!ores_native_actor_scheduler_stop_sealed(scheduler_, actor_))
            return false;
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
