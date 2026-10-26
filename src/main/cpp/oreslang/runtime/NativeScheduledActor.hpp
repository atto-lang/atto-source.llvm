#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace oreslang::runtime {

/** Trusted local runtime factory; never a guest constructor or numeric cast.
 *
 * Spawn and adopt are one authority path. Arbitrary equal routing labels
 * cannot certify identity. Scheduler storage must outlive its group and all
 * outstanding transactions.
 */
class NativeScheduledActor final {
public:
    static NativeActorRef spawn(NativeActorGroup& group,
                                OresNativeActorScheduler* scheduler,
                                std::size_t slot,
                                std::uint64_t routing_label) {
        if (!scheduler || !routing_label)
            throw std::invalid_argument(
                    "scheduled actor requires scheduler and routing label");
        const auto native = ores_native_actor_scheduler_spawn(scheduler, slot);
        if (!native.scheduler_domain || !native.generation)
            throw std::logic_error("scheduled actor spawn rejected");
        try {
            return group.adopt(
                    group.runtime_id(), routing_label, native.generation,
                    [scheduler, native] {
                        (void)ores_native_actor_scheduler_stop(
                                scheduler, native);
                    },
                    scheduler,
                    native);
        } catch (...) {
            (void)ores_native_actor_scheduler_stop(scheduler, native);
            throw;
        }
    }

private:
    NativeScheduledActor() = delete;
};

} // namespace oreslang::runtime
