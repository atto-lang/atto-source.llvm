#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/OresTaskBoundStateRegion.h"

#include <atomic>
#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

/**
 * Pins one exact actor incarnation while a state-region ticket is live.
 *
 * The state-region gate owns read/write admission; NativeActorRef's resource
 * loan owns physical actor-resource lifetime. A ticket may migrate carriers,
 * but its logical execution/generation remains checked by the C gate.
 *
 * Destruction intentionally does NOT release an active pin. Losing this
 * wrapper before the ticket physically retires must fail closed by leaving the
 * actor resource loan outstanding, preventing premature arena reclamation.
 *
 * The region object and arena pointer are trusted runtime-owned storage and
 * must outlive this pin. Guest code receives neither pointer.
 */
class StateRegionActorLifetimePin final {
public:
    StateRegionActorLifetimePin(
            NativeActorRef actor,
            OresActorArena& arena,
            ores_task_bound_state_region& region,
            ores_std_ticket ticket)
        : actor_(std::move(actor)),
          arena_(&arena),
          region_(&region),
          ticket_(ticket) {
        // Reserve physical actor ownership BEFORE inspecting arena identity
        // or the state ticket. Otherwise a racing finalizer can reclaim the
        // actor arena between a successful ticket query and the late retain.
        // A terminal actor rejects this reservation before any arena deref.
        actor_.retain_resource_loan();
        try {
            OresArenaIdentity identity{};
            if (!ores_actor_arena_identity(arena_, &identity)
                    || identity.owner != actor_.actor_id()
                    || identity.incarnation != actor_.generation()
                    || !ores_state_region_matches_actor_arena(region_, arena_))
                throw std::invalid_argument(
                    "state-region actor lifetime pin identity mismatch");

            const auto status = ores_state_region_ticket_retired(region_, ticket_);
            if (status == ORES_STD_SYNC_RETRY)
                throw std::runtime_error("state-region metadata busy");
            if (status != ORES_STD_SYNC_EXECUTING)
                throw std::invalid_argument(
                    "state-region lifetime pin requires a live ticket");

            active_.store(true, std::memory_order_release);
        } catch (...) {
            // No constructor failure may leak or fabricate a resource loan.
            // The native arena/region still require their trusted caller's
            // own pre-construction lifetime guarantee.
            actor_.release_resource_loan();
            throw;
        }
    }

    StateRegionActorLifetimePin(const StateRegionActorLifetimePin&) = delete;
    StateRegionActorLifetimePin& operator=(const StateRegionActorLifetimePin&) = delete;
    StateRegionActorLifetimePin(StateRegionActorLifetimePin&&) = delete;
    StateRegionActorLifetimePin& operator=(StateRegionActorLifetimePin&&) = delete;

    ~StateRegionActorLifetimePin() noexcept = default;

    bool release_after_ticket_retirement() noexcept {
        if (!active_.load(std::memory_order_acquire)) return false;

        // Claim the only retirement probe BEFORE touching region_. A delayed
        // loser must never dereference actor-owned state after the winner can
        // release the last resource loan and make physical reclamation legal.
        bool expected = false;
        if (!release_probe_claimed_.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
            return false;

        const auto status = ores_state_region_ticket_retired(region_, ticket_);
        if (status != ORES_STD_SYNC_GRANTED) {
            // The actor loan is still held and active_ remains true. Re-open
            // the probe lane so a later completion observation can retry.
            release_probe_claimed_.store(false, std::memory_order_release);
            return false;
        }

        // Keep active_ true until after the resource loan is actually
        // relinquished. This preserves its meaning for lifecycle observers:
        // active()==true always implies this pin still owns actor lifetime.
        actor_.release_resource_loan();
        active_.store(false, std::memory_order_release);
        return true;
    }

    bool active() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

private:
    NativeActorRef actor_;
    OresActorArena* arena_ = nullptr;
    ores_task_bound_state_region* region_ = nullptr;
    ores_std_ticket ticket_{};
    std::atomic<bool> active_{false};
    std::atomic<bool> release_probe_claimed_{false};
};

} // namespace oreslang::runtime
