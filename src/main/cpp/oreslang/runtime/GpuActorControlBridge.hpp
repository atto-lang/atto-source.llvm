#pragma once

#include "oreslang/runtime/GpuDispatchCoordinator.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>

namespace oreslang::gpu {

/**
 * ActorGroup integration adapter with no second actor registry. The Group
 * template must be the authoritative native runtime ActorGroup (#54) and
 * provide enqueue_control(ref, callback). The group must have shared
 * ownership until all in-flight device completions are reconciled.
 */
template <typename Group>
class GpuActorControlBridge {
public:
    enum class Route { waiting, posted, group_retired, slot_busy, actor_executed };
    struct Receipt {
        std::atomic<Route> route{Route::waiting};
        std::atomic<bool> actor_handler_called{false};
    };
    using ActorRef = typename Group::ActorRef;
    using ActorHandler = std::function<void(std::exception_ptr)>;

    static std::shared_ptr<Receipt> attach(
            GpuDispatchCoordinator::Submission& submission,
            const std::shared_ptr<Group>& group,
            const ActorRef& actor,
            ActorHandler actor_handler) {
        if (!group || !actor_handler)
            throw std::invalid_argument("active ActorGroup and actor handler required");
        auto receipt = std::make_shared<Receipt>();
        std::weak_ptr<Group> weak_group(group);
        // The callback runs on a device/provider carrier and must ONLY
        // queue CONTROL. No user actor handler is invoked on this thread.
        submission.on_done([weak_group, actor, receipt, handler = std::move(actor_handler)](
                std::exception_ptr failure) {
            auto owner = weak_group.lock();
            if (!owner) {
                receipt->route.store(Route::group_retired);
                return;
            }
            bool accepted = owner->enqueue_control(actor,
                    [receipt, handler, failure] {
                        receipt->actor_handler_called.store(true);
                        receipt->route.store(Route::actor_executed);
                        handler(failure);
                    });
            receipt->route.store(accepted ? Route::posted : Route::slot_busy);
        });
        return receipt;
    }
};

} // namespace oreslang::gpu
