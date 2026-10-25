#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace oreslang::runtime {

/** Trusted local runtime factory; never a guest constructor or numeric cast.
 *
 * Spawn and adopt are one authority path. There is no bind(existing_ref,
 * numeric_actor) API: arbitrary equal routing labels cannot certify identity.
 * Scheduler storage must outlive its group and all outstanding transactions.
 * Startup/finalization notifications remain the trusted executor's job and
 * must correspond to actual scheduler readiness/physical completion.
 */
class NativeScheduledActor final {
public:
    static NativeActorRef spawn(NativeActorGroup& group,
                               OresNativeActorScheduler* scheduler,
                               std::size_t slot,
                               std::uint64_t routing_label) {
        if (!scheduler || !routing_label)
            throw std::invalid_argument("scheduled actor requires scheduler and routing label");
        const auto native = ores_native_actor_scheduler_spawn(scheduler, slot);
        if (!native.scheduler_domain || !native.generation)
            throw std::logic_error("scheduled actor spawn rejected");
        try {
            auto ref = group.adopt(group.runtime_id(), routing_label,
                native.generation, [scheduler, native] {
                    // STOP is idempotent at the host integration boundary.
                    // The executor still supplies the physical DONE receipt.
                    (void)ores_native_actor_scheduler_stop(scheduler, native);
                });
            {
                std::lock_guard<std::mutex> guard(ref.ticket_->mutex);
                ref.ticket_->scheduler = scheduler;
                ref.ticket_->scheduler_actor = native;
            }
            return ref;
        } catch (...) {
            // Adoption failure must not leave a live orphaned scheduler lane.
            (void)ores_native_actor_scheduler_stop(scheduler, native);
            throw;
        }
    }

private:
    NativeScheduledActor() = delete;
};

} // namespace oreslang::runtime
