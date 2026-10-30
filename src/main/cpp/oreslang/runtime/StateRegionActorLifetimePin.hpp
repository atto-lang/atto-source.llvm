#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/ActorArena.h"
#include "oreslang/runtime/OresTaskBoundStateRegion.h"

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

        actor_.retain_resource_loan();
        active_ = true;
    }

    StateRegionActorLifetimePin(const StateRegionActorLifetimePin&) = delete;
    StateRegionActorLifetimePin& operator=(const StateRegionActorLifetimePin&) = delete;
    StateRegionActorLifetimePin(StateRegionActorLifetimePin&&) = delete;
    StateRegionActorLifetimePin& operator=(StateRegionActorLifetimePin&&) = delete;

    ~StateRegionActorLifetimePin() noexcept = default;

    bool release_after_ticket_retirement() noexcept {
        if (!active_) return false;
        const auto status = ores_state_region_ticket_retired(region_, ticket_);
        if (status != ORES_STD_SYNC_GRANTED) return false;
        actor_.release_resource_loan();
        active_ = false;
        return true;
    }

    bool active() const noexcept { return active_; }

private:
    NativeActorRef actor_;
    OresActorArena* arena_ = nullptr;
    ores_task_bound_state_region* region_ = nullptr;
    ores_std_ticket ticket_{};
    bool active_ = false;
};

} // namespace oreslang::runtime
