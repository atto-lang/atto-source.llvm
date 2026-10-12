#pragma once

#include "oreslang/runtime/ActorStateReadWriteContinuations.hpp"

#include <cstddef>
#include <mutex>
#include <type_traits>
#include <utility>

namespace oreslang::runtime {

/**
 * Trusted adapter between durable RW continuation notifications and a
 * bounded scheduler CONTROL admission callback.
 *
 * The callback must be nonblocking, noexcept, and return true ONLY after
 * the scheduler has recorded an actual admission receipt. It must never
 * run guest code, invoke actor methods inline, or use the reserved panic
 * supervision slot for ordinary RW wakeups. A false result preserves the
 * notification for a future pump. The production runtime must supply a
 * separate scheduler-authorized CONTROL admission path.
 *
 * Notification delivery is intentionally not permission to execute: before
 * dispatching each READY continuation the consumer must re-check
 * registry.admitted(continuation_id, mode). Retirement can revoke a grant
 * after an admission receipt but before that queued CONTROL item executes.
 */
class ActorStateReadWriteControlPump final {
public:
    using Registry = ActorStateReadWriteContinuations;
    using Notification = Registry::Notification;

    explicit ActorStateReadWriteControlPump(Registry& registry)
        : registry_(registry) {}

    ActorStateReadWriteControlPump(const ActorStateReadWriteControlPump&) = delete;
    ActorStateReadWriteControlPump& operator=(const ActorStateReadWriteControlPump&) = delete;

    template <class TryAdmit>
    [[nodiscard]] std::size_t pump(TryAdmit&& try_admit, std::size_t budget = 64) {
        static_assert(std::is_nothrow_invocable_r_v<bool, TryAdmit&, const Notification&>,
                      "scheduler CONTROL admission must return bool without throwing");
        if (budget == 0) return 0;
        std::lock_guard<std::mutex> pump_lock(pump_mu_);
        // Snapshot does not consume; registry metadata locks are not held
        // during CONTROL queue admission.
        const auto notifications = registry_.pending_notifications();
        std::size_t receipts = 0;
        for (const auto& notification : notifications) {
            if (receipts == budget || !try_admit(notification)) break;
            // If cancellation raced admission, the READY receipt is stale;
            // the guest consumer must fence it using admitted() before work.
            (void)registry_.acknowledge(notification.continuation_id,
                                        notification.status);
            ++receipts;
        }
        return receipts;
    }

private:
    Registry& registry_;
    std::mutex pump_mu_;
};

} // namespace oreslang::runtime
