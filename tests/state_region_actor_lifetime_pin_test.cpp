#include "oreslang/runtime/StateRegionActorLifetimePin.hpp"
#include "oreslang/runtime/NativeActorRetirementBarrier.hpp"

#include <cassert>
#include <chrono>
#include <memory>
#include <stdexcept>

using namespace oreslang::runtime;

static ores_state_task_identity execution_identity() {
    ores_state_task_identity out{};
    out.logical.runtime_domain = 901;
    out.logical.execution = 77;
    out.handle.slot = 3;
    out.handle.generation = 5;
    return out;
}

int main() {
    constexpr std::uint64_t actor_id = 41;
    constexpr std::uint64_t generation = 7;

    NativeActorResourceQuarantine quarantine(2);
    NativeActorGroup group(711, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(
        actor_id, generation);

    OresActorArena* arena =
        ores_actor_arena_open(actor_id, generation, 2, 64, 128);
    assert(arena != nullptr);

    int freed = 0;
    registry->register_resource(
        ActorResourceRegistry::Kind::native_allocation,
        [arena, &freed] {
            ores_actor_arena_retire(arena);
            if (ores_actor_arena_try_destroy(arena) != 1)
                throw std::runtime_error(
                    "actor arena still pinned at physical reclamation");
            ++freed;
        });

    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(
        711, actor_id, generation, [&] { barrier->retire(); });
    NativeActorRetirementBarrier owned(
        registry, group.runtime_finalizer(ref), quarantine);
    barrier = &owned;
    assert(group.mark_ready(ref));

    ores_task_bound_state_region region =
        ORES_TASK_BOUND_STATE_REGION_UNBOUND_INIT;
    auto bind = ores_state_region_bind_actor_arena(&region, arena);
    assert(bind == ORES_STD_SYNC_GRANTED);
    assert(ores_state_region_matches_actor_arena(&region, arena));

    const auto owner = execution_identity();
    auto request = ores_state_region_request(
        &region, owner, ORES_STD_WRITE);
    assert(request.code == ORES_STD_SYNC_GRANTED);
    assert(request.ticket.sequence != 0);

    StateRegionActorLifetimePin pin(
        ref, *arena, region, request.ticket);
    assert(pin.active());
    assert(ref.outstanding_resource_loans() == 1);
    // A syntactically valid ticket of the correct scope must still prove
    // exact live-binding identity; simply sharing an arena is insufficient.
    auto forged_ticket = request.ticket;
    ++forged_ticket.sequence;
    bool forged_rejected = false;
    try {
        StateRegionActorLifetimePin forged(
            ref, *arena, region, forged_ticket);
    } catch (const std::invalid_argument&) {
        forged_rejected = true;
    }
    assert(forged_rejected);
    assert(ref.outstanding_resource_loans() == 1);
    // A different region cannot launder an otherwise valid actor ticket.
    ores_task_bound_state_region unbound_region =
        ORES_TASK_BOUND_STATE_REGION_UNBOUND_INIT;
    bool foreign_region_rejected = false;
    try {
        StateRegionActorLifetimePin foreign(
            ref, *arena, unbound_region, request.ticket);
    } catch (const std::invalid_argument&) {
        foreign_region_rejected = true;
    }
    assert(foreign_region_rejected);
    assert(ref.outstanding_resource_loans() == 1);
    assert(ores_state_region_start(&region, owner, request.ticket)
           == ORES_STD_SYNC_GRANTED);

    // A different actor generation cannot claim the live region ticket.
    NativeActorGroup stale_group(712, 1);
    auto stale = stale_group.adopt(
        712, actor_id, generation + 1, [] {});
    bool rejected = false;
    try {
        StateRegionActorLifetimePin wrong(
            stale, *arena, region, request.ticket);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(stale.outstanding_resource_loans() == 0);
    stale_group.close();
    assert(stale.outstanding_resource_loans() == 0);
    stale.get_done().get();
    // Once this unrelated actor has reached a terminal resource state,
    // attempting a lifetime pin must fail *before* reading any arena fields.
    bool terminal_rejected = false;
    try {
        StateRegionActorLifetimePin terminal(
            stale, *arena, region, request.ticket);
    } catch (const std::logic_error&) {
        terminal_rejected = true;
    }
    assert(terminal_rejected);
    assert(stale.outstanding_resource_loans() == 0);

    // Logical stop cannot physically reclaim the arena while the ticket lives.
    group.close();
    assert(owned.poll() ==
        NativeActorRetirementBarrier::Poll::not_admitted);
    assert(freed == 0);
    assert(ref.get_done().wait_for(std::chrono::milliseconds(0))
        == std::future_status::timeout);
    assert(!pin.release_after_ticket_retirement());
    assert(ref.outstanding_resource_loans() == 1);
    // A logical STOP may not release a PHYSICALLY EXECUTING task ticket.
    assert(ores_state_region_release(&region, owner, request.ticket).code
           == ORES_STD_SYNC_EXECUTING);
    assert(ores_state_region_ready_to_suspend(&region, owner, request.ticket)
           == ORES_STD_SYNC_EXECUTING);
    assert(freed == 0);
    assert(ores_state_region_end(&region, owner, request.ticket)
           == ORES_STD_SYNC_GRANTED);

    // Retire the exact state ticket. Only then may the actor lifetime pin drop.
    auto released = ores_state_region_release(
        &region, owner, request.ticket);
    assert(released.code == ORES_STD_SYNC_GRANTED);
    assert(ores_state_region_ticket_retired(
        &region, request.ticket) == ORES_STD_SYNC_GRANTED);
    assert(pin.release_after_ticket_retirement());
    assert(!pin.active());
    assert(ref.outstanding_resource_loans() == 0);
    // A once-live but now retired exact ticket cannot reacquire arena
    // lifetime, even before the resource registry's final close runs.
    bool retired_ticket_rejected = false;
    try {
        StateRegionActorLifetimePin replay(
            ref, *arena, region, request.ticket);
    } catch (const std::invalid_argument&) {
        retired_ticket_rejected = true;
    }
    assert(retired_ticket_rejected);
    assert(ref.outstanding_resource_loans() == 0);

    assert(owned.poll() == NativeActorRetirementBarrier::Poll::completed);
    assert(freed == 1);
    ref.get_done().get();

    // Release is linear and physical reclamation occurs exactly once.
    assert(!pin.release_after_ticket_retirement());
    assert(owned.poll() == NativeActorRetirementBarrier::Poll::completed);
    assert(freed == 1);

    return 0;
}
