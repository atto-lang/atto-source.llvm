#pragma once

#include "oreslang/runtime/ActorStateReadWriteContinuations.hpp"

#include <cstddef>
#include <atomic>
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
        // std::mutex::try_lock is undefined for same-thread recursion.
        // An atomic admission flag makes reentrant and concurrent pump calls
        // return immediately, without blocking the scheduler carrier.
        if (pumping_.test_and_set(std::memory_order_acquire)) return 0;
        struct PumpExit {
            std::atomic_flag& flag;
            ~PumpExit() { flag.clear(std::memory_order_release); }
        } guard{pumping_};
        // Snapshot does not consume; registry metadata locks are not held
        // during CONTROL queue admission.
        const auto notifications = registry_.pending_notifications();
        std::size_t receipts = 0;
        for (const auto& notification : notifications) {
            if (receipts == budget || !try_admit(notification)) break;
            // If cancellation raced admission, the READY receipt is stale;
            // the guest consumer must fence it using admitted() before work.
            (void)registry_.acknowledge(notification);
            ++receipts;
        }
        return receipts;
    }

private:
    Registry& registry_;
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
};

} // namespace oreslang::runtime
