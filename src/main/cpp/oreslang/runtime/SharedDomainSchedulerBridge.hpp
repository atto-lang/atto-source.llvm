#pragma once

#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/SharedDomainArena.hpp"
#include "oreslang/runtime/SharedDomainReadWriteGate.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/*
 * Trusted bridge between scheduler-owned actor identity and one
 * SharedDomainArena. Guest code never supplies actor IDs/incarnations.
 *
 * v1 host prototype limitation: one scheduler owns the actor members of a
 * bridge. Cross-scheduler / cross-process shared-domain transport requires a
 * separate authenticated principal layer and is deliberately unsupported.
 */
class SharedDomainSchedulerBridge final {
public:
    using Arena = SharedDomainArena;
    using Gate = SharedDomainReadWriteGate;
    using Mode = Gate::Mode;

    class Permit final {
        friend class SharedDomainSchedulerBridge;
        Arena::Grant grant_;
        OresNativeActorIdentity identity_;
        OresNativeActorRef actor_;
        Permit(Arena::Grant grant, OresNativeActorIdentity identity,
               OresNativeActorRef actor)
            : grant_(grant), identity_(identity), actor_(actor) {}
    public:
        Permit(const Permit&) = default;
        Permit& operator=(const Permit&) = default;
        std::uint64_t domain_id() const noexcept { return grant_.domain_id(); }
    };

    struct Request final {
        Gate::Token token;
        Mode mode;
        Gate::Admission admission;
        explicit operator bool() const noexcept { return static_cast<bool>(token); }
        bool acquired() const noexcept { return admission == Gate::Admission::Acquired; }
    };

private:
    class CompletionGuard final {
    public:
        CompletionGuard(SharedDomainSchedulerBridge& bridge, Gate::Token token,
                        OresNativeActorIdentity owner) noexcept
            : bridge_(&bridge), token_(token), owner_(owner) {}
        CompletionGuard(const CompletionGuard&) = delete;
        CompletionGuard& operator=(const CompletionGuard&) = delete;
        CompletionGuard(CompletionGuard&&) = delete;
        CompletionGuard& operator=(CompletionGuard&&) = delete;
        ~CompletionGuard() noexcept {
            if (!bridge_) return;
            try {
                bridge_->complete_after_physical_lease(token_, owner_);
            } catch (...) {
                std::terminate();
            }
        }
    private:
        SharedDomainSchedulerBridge* bridge_;
        Gate::Token token_;
        OresNativeActorIdentity owner_;
    };

public:
    /*
     * Access is lexical and non-movable. Member declaration order is
     * intentional: Arena::Lease is destroyed before CompletionGuard, so the
     * physical reader/writer lock is released before the next logical ticket
     * is published to the scheduler.
     */
    class Access final {
        friend class SharedDomainSchedulerBridge;
        CompletionGuard completion_;
        Arena::Lease lease_;
        Mode mode_;

        Access(SharedDomainSchedulerBridge& bridge, const Permit& permit,
               Gate::Token token, OresNativeActorIdentity owner, Mode mode)
            : completion_(bridge, token, owner),
              lease_(mode == Mode::Read
                    ? bridge.arena_.read(owner.principal, owner.incarnation, permit.grant_)
                    : bridge.arena_.write(owner.principal, owner.incarnation, permit.grant_)),
              mode_(mode) {}
    public:
        Access(const Access&) = delete;
        Access& operator=(const Access&) = delete;
        Access(Access&&) = delete;
        Access& operator=(Access&&) = delete;
        ~Access() = default;

        Arena::Handle allocate(std::size_t bytes) {
            if (mode_ != Mode::Write)
                throw std::logic_error("shared-domain read access cannot allocate");
            return lease_.allocate(bytes);
        }
        void read(Arena::Handle handle, void* out, std::size_t bytes) const {
            lease_.read(handle, out, bytes);
        }
        void write(Arena::Handle handle, const void* data, std::size_t bytes) {
            if (mode_ != Mode::Write)
                throw std::logic_error("shared-domain read access cannot write");
            lease_.write(handle, data, bytes);
        }
        void drop(Arena::Handle handle) {
            if (mode_ != Mode::Write)
                throw std::logic_error("shared-domain read access cannot drop");
            lease_.drop(handle);
        }
        Mode mode() const noexcept { return mode_; }
    };

    SharedDomainSchedulerBridge(Arena& arena, OresNativeActorScheduler* scheduler)
        : arena_(arena), scheduler_(scheduler) {
        if (!scheduler_)
            throw std::invalid_argument("shared-domain bridge requires scheduler");
    }
    SharedDomainSchedulerBridge(const SharedDomainSchedulerBridge&) = delete;
    SharedDomainSchedulerBridge& operator=(const SharedDomainSchedulerBridge&) = delete;

    /*
     * CONTROL membership uses a live ActorRef, but converts it immediately to
     * scheduler-owned non-recycling identity. Subsequent byte access never
     * accepts that ref or any guest-provided numeric identity.
     */
    [[nodiscard]] Permit grant_member(
            Arena::Control& control, OresNativeActorRef actor,
            bool allow_read, bool allow_write) {
        OresNativeActorIdentity identity{0, 0};
        if (!ores_native_actor_scheduler_resolve_actor_identity(
                scheduler_, actor, &identity))
            throw std::logic_error("shared-domain grant requires live scheduler actor");

        std::lock_guard<std::mutex> lock(mu_);
        std::size_t slot = members_.size();
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (members_[i].active
                    && same_identity(members_[i].identity, identity))
                throw std::logic_error("actor already has shared-domain membership");
            if (!members_[i].active && slot == members_.size()) slot = i;
        }
        if (slot == members_.size())
            throw std::out_of_range("shared-domain bridge membership quota");

        auto grant = arena_.grant(
            control, identity.principal, identity.incarnation,
            allow_read, allow_write);
        members_[slot] = Member{actor, identity, true};
        return Permit(grant, identity, actor);
    }

    /*
     * Request is always made by the scheduler's actual current actor turn.
     * It never blocks. Acquired tickets may enter immediately; queued tickets
     * must suspend and resume only from the exact capability-wake envelope.
     */
    [[nodiscard]] Request request(const Permit& permit, Mode mode) {
        const auto current = require_current_identity();
        require_permit_owner(permit, current);
        const auto request = gate_.request(current, mode);
        return {request.token, mode, request.admission};
    }

    [[nodiscard]] Access enter(const Permit& permit, const Request& request) {
        if (!request.acquired())
            throw std::logic_error("queued shared-domain request requires scheduler wake");
        return claim_and_enter(permit, request.token, request.mode);
    }

    [[nodiscard]] Access resume(
            const Permit& permit, OresNativeActorCapabilityWake wake, Mode mode) {
        if (wake.status != ORES_NATIVE_CAPABILITY_READY
                || wake.scope != gate_.scope()
                || !ores_native_actor_scheduler_is_current_capability_wake(
                    scheduler_, wake))
            throw std::logic_error("shared-domain resume requires exact scheduler wake");
        return claim_and_enter(
            permit, Gate::Token(wake.scope, wake.ticket), mode);
    }

    /*
     * Cancel only a request that has not become active. This is a trusted
     * runtime path used while unwinding a suspended continuation.
     */
    void cancel_pending(const Permit& permit, const Request& request) {
        if (request.acquired())
            throw std::logic_error("active shared-domain access must unwind lexically");
        auto ready = gate_.cancel_pending(
            request.token, permit.identity_);
        enqueue_ready(ready);
        (void)pump_wakes();
    }

    /*
     * Lifetime-only completion ownership: the returned pin has no memory API.
     * A generic registrar can atomically take ownership of a move-only cleanup
     * callback. If registration throws, RAII releases the pin immediately.
     */
    [[nodiscard]] Arena::LifetimePin pin_current(const Permit& permit) {
        const auto current = require_current_identity();
        require_permit_owner(permit, current);
        return arena_.pin(
            current.principal, current.incarnation, permit.grant_);
    }

    template <class Register>
    void track_current_completion(const Permit& permit, Register&& registrar) {
        auto pin = pin_current(permit);
        auto release = [owned = std::move(pin)]() mutable noexcept {
            owned.release();
        };
        std::forward<Register>(registrar)(std::move(release));
    }

    /*
     * Retry durable wake notifications without running guest code inline.
     * Failure means the target actor wake slot is currently occupied/stopped;
     * the notification remains queued for a later CONTROL/runtime pump.
     */
    [[nodiscard]] std::size_t pump_wakes(std::size_t budget = 64) {
        if (budget == 0) return 0;
        if (pumping_.test_and_set(std::memory_order_acquire)) return 0;
        struct PumpExit {
            std::atomic_flag& flag;
            ~PumpExit() { flag.clear(std::memory_order_release); }
        } exit{pumping_};

        std::size_t delivered = 0;
        std::size_t attempts = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            attempts = pending_wakes_.size();
        }
        while (attempts-- && delivered < budget) {
            WakeRecord record;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (pending_wakes_.empty()) break;
                record = pending_wakes_.front();
                pending_wakes_.pop_front();
            }
            const OresNativeActorRef actor = actor_for(record.ready.owner);
            const OresNativeActorCapabilityWake wake{
                record.ready.token.scope(),
                record.ready.token.sequence(),
                ORES_NATIVE_CAPABILITY_READY
            };
            if (actor.scheduler_domain
                    && ores_native_actor_scheduler_publish_capability_wake(
                        scheduler_, actor, wake)) {
                ++delivered;
            } else {
                std::lock_guard<std::mutex> lock(mu_);
                pending_wakes_.push_back(record);
            }
        }
        return delivered;
    }

    [[nodiscard]] std::size_t waiting() const { return gate_.waiting(); }

private:
    struct Member {
        OresNativeActorRef actor{0, 0, 0};
        OresNativeActorIdentity identity{0, 0};
        bool active = false;
    };
    struct WakeRecord {
        Gate::Ready ready{};
    };

    static bool same_identity(
            OresNativeActorIdentity lhs, OresNativeActorIdentity rhs) noexcept {
        return lhs.principal == rhs.principal
            && lhs.incarnation == rhs.incarnation;
    }

    OresNativeActorIdentity require_current_identity() const {
        OresNativeActorIdentity current{0, 0};
        if (!ores_native_actor_scheduler_current_actor_identity(
                scheduler_, &current))
            throw std::logic_error(
                "shared-domain access requires current scheduler actor turn");
        return current;
    }

    static void require_permit_owner(
            const Permit& permit, OresNativeActorIdentity current) {
        if (!same_identity(permit.identity_, current))
            throw std::logic_error("shared-domain permit belongs to another actor incarnation");
    }

    [[nodiscard]] Access claim_and_enter(
            const Permit& permit, Gate::Token token, Mode mode) {
        const auto current = require_current_identity();
        require_permit_owner(permit, current);
        if (!gate_.try_claim(token, current, mode))
            throw std::logic_error("stale/replayed shared-domain admission");
        return Access(*this, permit, token, current, mode);
    }

    void complete_after_physical_lease(
            Gate::Token token, OresNativeActorIdentity owner) {
        auto ready = gate_.release(token, owner);
        enqueue_ready(ready);
        (void)pump_wakes();
    }

    void enqueue_ready(const std::vector<Gate::Ready>& ready) {
        if (ready.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& item : ready)
            pending_wakes_.push_back({item});
    }

    OresNativeActorRef actor_for(OresNativeActorIdentity identity) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& member : members_) {
            if (member.active && same_identity(member.identity, identity))
                return member.actor;
        }
        return {0, 0, 0};
    }

    Arena& arena_;
    OresNativeActorScheduler* scheduler_;
    Gate gate_;
    mutable std::mutex mu_;
    std::array<Member, Arena::Members> members_{};
    std::deque<WakeRecord> pending_wakes_;
    std::atomic_flag pumping_ = ATOMIC_FLAG_INIT;
};

} // namespace oreslang::runtime
