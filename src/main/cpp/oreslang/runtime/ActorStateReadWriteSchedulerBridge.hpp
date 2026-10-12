#pragma once

#include "oreslang/runtime/ActorStateReadWriteContinuations.hpp"
#include "oreslang/runtime/ActorStateReadWriteControlPump.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>

namespace oreslang::runtime {

/**
 * Trusted native backend bridge from continuation admission to an actor's
 * dedicated scheduler-owned state wake slot. Never uses supervisor CONTROL,
 * never runs guest code on the submitting thread, and never parks a carrier.
 *
 * Lifetime: externally fence the scheduler, actor ref, registry and bridge
 * together. Drain all admitted callbacks BEFORE destroying the bridge.
 * Guest callbacks run only after try_claim_ready verifies both the physical
 * actor execution claim and the one-shot continuation ticket.
 */
class ActorStateReadWriteSchedulerBridge final {
public:
    using Registry = ActorStateReadWriteContinuations;
    using Notification = Registry::Notification;
    using Mode = Registry::Mode;

    ActorStateReadWriteSchedulerBridge(
            Registry& registry, OresNativeActorScheduler* scheduler,
            OresNativeActorRef actor)
            : registry_(registry), pump_(registry), scheduler_(scheduler), actor_(actor) {
        if (!scheduler_ || !actor_.scheduler_domain || !actor_.generation)
            throw std::invalid_argument("RW scheduler bridge requires live actor authority");
    }
    ActorStateReadWriteSchedulerBridge(const ActorStateReadWriteSchedulerBridge&) = delete;
    ActorStateReadWriteSchedulerBridge& operator=(const ActorStateReadWriteSchedulerBridge&) = delete;

    [[nodiscard]] std::size_t pump(std::size_t budget = 1) {
        return pump_.pump([this](const Notification& notification) noexcept -> bool {
            if (notification.incarnation != registry_.incarnation()) return false;
            const std::uint64_t status = notification.status == Registry::Status::Ready
                    ? ORES_NATIVE_RW_READY
                    : notification.status == Registry::Status::Cancelled
                    ? ORES_NATIVE_RW_CANCELLED : 0;
            if (!status) return false;
            const OresNativeActorStateWake wake = {
                notification.incarnation, notification.continuation_id, status
            };
            return ores_native_actor_scheduler_publish_state_wake(
                    scheduler_, actor_, wake) != 0;
        }, budget);
    }

    /**
     * Call from the scheduler-owned state wake callback ONLY. This check
     * must be adjacent to actual guest entry, not just queue admission.
     * It fences forged actor refs and actor-incarnation ABA, and atomically
     * claims the continuation even if the producer hasn't acknowledged yet.
     */
    [[nodiscard]] bool try_enter(
            OresNativeActorStateWake wake, Mode expected_mode) {
        if (wake.status != ORES_NATIVE_RW_READY
                || !ores_native_actor_scheduler_is_current_state_wake(
                    scheduler_, actor_, wake))
            return false;
        return registry_.try_claim_ready({
            wake.incarnation, wake.continuation_id, Registry::Status::Ready
        }, expected_mode);
    }

    // Must be called on that same actor turn after guest code has unwound.
    void complete(std::uint64_t continuation_id) {
        const OresNativeActorStateWake active = {
            registry_.incarnation(), continuation_id, ORES_NATIVE_RW_READY
        };
        if (!ores_native_actor_scheduler_is_current_state_wake(
                scheduler_, actor_, active))
            throw std::logic_error(
                    "state completion requires exact owning RW wake callback");
        registry_.complete_dispatched(registry_.incarnation(), continuation_id);
    }

    /**
     * Runtime callback scope: claim exactly one scheduler-delivered READY
     * ticket and release it on every normal/exceptional return. Guest
     * exceptions must be caught inside the C++ callback wrapper; they
     * must never unwind through the native C scheduler ABI. Destructors
     * fail closed on an unexpected ownership violation.
     *
     * The scope does not authorize actor execution on another carrier:
     * construction is legal only in the exact scheduler wake callback.
     */
    class ScopedWake final {
    public:
        ScopedWake(ActorStateReadWriteSchedulerBridge& bridge,
                   OresNativeActorStateWake wake, Mode mode)
                : bridge_(bridge.try_enter(wake, mode) ? &bridge : nullptr),
                  continuation_id_(wake.continuation_id) {}

        ScopedWake(const ScopedWake&) = delete;
        ScopedWake& operator=(const ScopedWake&) = delete;
        ScopedWake(ScopedWake&&) = delete;
        ScopedWake& operator=(ScopedWake&&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept {
            return bridge_ != nullptr;
        }

        void finish() {
            if (!bridge_) throw std::logic_error("RW wake scope is not active");
            bridge_->complete(continuation_id_);
            bridge_ = nullptr;
        }

        ~ScopedWake() noexcept {
            if (!bridge_) return;
            try {
                bridge_->complete(continuation_id_);
            } catch (...) {
                std::terminate();  // loss of a claimed ticket is fatal
            }
        }
    private:
        ActorStateReadWriteSchedulerBridge* bridge_;
        std::uint64_t continuation_id_;
    };

    // CANCELLED is trusted terminal bookkeeping, never authority to run code.
    [[nodiscard]] bool is_own_cancellation(OresNativeActorStateWake wake) const noexcept {
        return wake.status == ORES_NATIVE_RW_CANCELLED
                && wake.incarnation == registry_.incarnation()
                && ores_native_actor_scheduler_is_current_state_wake(
                    scheduler_, actor_, wake);
    }

private:
    Registry& registry_;
    ActorStateReadWriteControlPump pump_;
    OresNativeActorScheduler* scheduler_;
    OresNativeActorRef actor_;
};

} // namespace oreslang::runtime
