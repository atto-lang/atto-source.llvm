#include "oreslang/runtime/StateRegionActorLifetimePin.hpp"
#include "oreslang/runtime/NativeActorRetirementBarrier.hpp"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <memory>
#include <stdexcept>

// CTest/standalone Release builds must not silently remove the API calls
// inside assertions. This test exercises physical ticket transitions even
// when the external build defines NDEBUG.
#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        std::abort(); \
    } \
} while (false)

using namespace oreslang::runtime;

static ores_state_task_identity execution_identity() {
    ores_state_task_identity out{};
    out.logical.runtime_domain = 901;
    out.logical.execution = 77;
    out.handle.slot = 3;
    out.handle.generation = 5;
    return out;
}

// Repeats the completion race on fresh pins so the one-winner fence is proven
// by more than a single, usually uncontended, interleaving.
static void one_winner_release_stress() {
    constexpr std::uint64_t actor_id = 51;
    constexpr std::uint64_t generation = 9;
    constexpr int rounds = 2000;
    constexpr int contenders = 8;

    NativeActorGroup group(721, 1);
    auto ref = group.adopt(721, actor_id, generation, [] {});
    auto finalizer = group.runtime_finalizer(ref);
    CHECK(group.mark_ready(ref));
    OresActorArena* arena =
        ores_actor_arena_open(actor_id, generation, 2, 64, 128);
    CHECK(arena != nullptr);
    ores_task_bound_state_region region =
        ORES_TASK_BOUND_STATE_REGION_UNBOUND_INIT;
    CHECK(ores_state_region_bind_actor_arena(&region, arena)
        == ORES_STD_SYNC_GRANTED);
    const auto owner = execution_identity();

    for (int round = 0; round < rounds; ++round) {
        auto request = ores_state_region_request(
            &region, owner, ORES_STD_WRITE);
        CHECK(request.code == ORES_STD_SYNC_GRANTED);
        StateRegionActorLifetimePin pin(ref, *arena, region, request.ticket);
        CHECK(ores_state_region_release(&region, owner, request.ticket).code
            == ORES_STD_SYNC_GRANTED);

        std::atomic<bool> start{false};
        std::atomic<int> receipts{0};
        std::vector<std::thread> observers;
        observers.reserve(contenders);
        for (int i = 0; i < contenders; ++i) {
            observers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire))
                    std::this_thread::yield();
                if (pin.release_after_ticket_retirement())
                    receipts.fetch_add(1, std::memory_order_relaxed);
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& observer : observers) observer.join();
        CHECK(receipts.load(std::memory_order_relaxed) == 1);
        CHECK(ref.outstanding_resource_loans() == 0);
    }

    group.close();
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    ref.get_done().get();
    ores_actor_arena_retire(arena);
    CHECK(ores_actor_arena_try_destroy(arena) == 1);
}

int main() {
    one_winner_release_stress();

    constexpr std::uint64_t actor_id = 41;
    constexpr std::uint64_t generation = 7;

    NativeActorResourceQuarantine quarantine(2);
    NativeActorGroup group(711, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(
        actor_id, generation);

    OresActorArena* arena =
        ores_actor_arena_open(actor_id, generation, 2, 64, 128);
    CHECK(arena != nullptr);

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
    CHECK(group.mark_ready(ref));

    ores_task_bound_state_region region =
        ORES_TASK_BOUND_STATE_REGION_UNBOUND_INIT;
    auto bind = ores_state_region_bind_actor_arena(&region, arena);
    CHECK(bind == ORES_STD_SYNC_GRANTED);
    CHECK(ores_state_region_matches_actor_arena(&region, arena));

    const auto owner = execution_identity();
    auto request = ores_state_region_request(
        &region, owner, ORES_STD_WRITE);
    CHECK(request.code == ORES_STD_SYNC_GRANTED);
    CHECK(request.ticket.sequence != 0);

    StateRegionActorLifetimePin pin(
        ref, *arena, region, request.ticket);
    CHECK(pin.active());
    CHECK(ref.outstanding_resource_loans() == 1);
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
    CHECK(forged_rejected);
    CHECK(ref.outstanding_resource_loans() == 1);
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
    CHECK(foreign_region_rejected);
    CHECK(ref.outstanding_resource_loans() == 1);
    CHECK(ores_state_region_start(&region, owner, request.ticket)
           == ORES_STD_SYNC_GRANTED);

    // A different actor generation cannot claim the live region ticket.
    NativeActorGroup stale_group(712, 1);
    auto stale = stale_group.adopt(
        712, actor_id, generation + 1, [] {});
    // DONE settles only through the trusted finalizer; closing the group
    // alone leaves stale.get_done() pending forever.
    auto stale_finalizer = stale_group.runtime_finalizer(stale);
    CHECK(stale_group.mark_ready(stale));
    bool rejected = false;
    try {
        StateRegionActorLifetimePin wrong(
            stale, *arena, region, request.ticket);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(stale.outstanding_resource_loans() == 0);
    stale_group.close();
    CHECK(stale.outstanding_resource_loans() == 0);
    CHECK(stale_finalizer.ready_for_resource_reclamation());
    CHECK(stale_finalizer.finalized());
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
    CHECK(terminal_rejected);
    CHECK(stale.outstanding_resource_loans() == 0);

    // Same generation, different actor id: the arena owner must match too.
    NativeActorGroup other_owner_group(713, 1);
    auto other_owner = other_owner_group.adopt(
        713, actor_id + 1, generation, [] {});
    auto other_owner_finalizer = other_owner_group.runtime_finalizer(other_owner);
    CHECK(other_owner_group.mark_ready(other_owner));
    bool other_owner_rejected = false;
    try {
        StateRegionActorLifetimePin wrong_owner(
            other_owner, *arena, region, request.ticket);
    } catch (const std::invalid_argument&) {
        other_owner_rejected = true;
    }
    CHECK(other_owner_rejected);
    CHECK(other_owner.outstanding_resource_loans() == 0);
    other_owner_group.close();
    CHECK(other_owner_finalizer.finalized());
    other_owner.get_done().get();

    // A live ticket from a region bound to a sibling arena of the same actor
    // incarnation must not pin this arena: the ticket and region agree with
    // each other, so only the region/arena match can reject it.
    OresActorArena* sibling_arena =
        ores_actor_arena_open(actor_id, generation, 2, 64, 128);
    CHECK(sibling_arena != nullptr);
    ores_task_bound_state_region sibling_region =
        ORES_TASK_BOUND_STATE_REGION_UNBOUND_INIT;
    CHECK(ores_state_region_bind_actor_arena(&sibling_region, sibling_arena)
        == ORES_STD_SYNC_GRANTED);
    auto sibling_request = ores_state_region_request(
        &sibling_region, owner, ORES_STD_WRITE);
    CHECK(sibling_request.code == ORES_STD_SYNC_GRANTED);
    bool sibling_rejected = false;
    try {
        StateRegionActorLifetimePin laundered(
            ref, *arena, sibling_region, sibling_request.ticket);
    } catch (const std::invalid_argument&) {
        sibling_rejected = true;
    }
    CHECK(sibling_rejected);
    CHECK(ref.outstanding_resource_loans() == 1);
    CHECK(ores_state_region_release(&sibling_region, owner, sibling_request.ticket).code
        == ORES_STD_SYNC_GRANTED);
    ores_actor_arena_retire(sibling_arena);
    CHECK(ores_actor_arena_try_destroy(sibling_arena) == 1);

    // Logical stop cannot physically reclaim the arena while the ticket lives.
    group.close();
    CHECK(owned.poll() ==
        NativeActorRetirementBarrier::Poll::not_admitted);
    CHECK(freed == 0);
    CHECK(ref.get_done().wait_for(std::chrono::milliseconds(0))
        == std::future_status::timeout);
    // Competing early observers must serialize the retirement probe and
    // leave the pin active when the exact ticket is still executing.
    constexpr int early_contenders = 32;
    std::atomic<bool> start_early_completion{false};
    std::atomic<int> early_release_receipts{0};
    std::vector<std::thread> early_observers;
    early_observers.reserve(early_contenders);
    for (int i = 0; i < early_contenders; ++i) {
        early_observers.emplace_back([&] {
            while (!start_early_completion.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (pin.release_after_ticket_retirement())
                early_release_receipts.fetch_add(1, std::memory_order_relaxed);
        });
    }
    start_early_completion.store(true, std::memory_order_release);
    for (auto& observer : early_observers) observer.join();
    CHECK(early_release_receipts.load(std::memory_order_relaxed) == 0);
    CHECK(pin.active());
    CHECK(ref.outstanding_resource_loans() == 1);
    // A logical STOP may not release a PHYSICALLY EXECUTING task ticket.
    CHECK(ores_state_region_release(&region, owner, request.ticket).code
           == ORES_STD_SYNC_EXECUTING);
    CHECK(ores_state_region_ready_to_suspend(&region, owner, request.ticket)
           == ORES_STD_SYNC_EXECUTING);
    CHECK(freed == 0);
    CHECK(ores_state_region_end(&region, owner, request.ticket)
           == ORES_STD_SYNC_GRANTED);

    // Retire the exact state ticket. Only then may the actor lifetime pin drop.
    auto released = ores_state_region_release(
        &region, owner, request.ticket);
    CHECK(released.code == ORES_STD_SYNC_GRANTED);
    CHECK(ores_state_region_ticket_retired(
        &region, request.ticket) == ORES_STD_SYNC_GRANTED);
    // Real concurrent completion observers may both see a retired ticket.
    // Exactly one may release the actor resource loan; all losing callbacks
    // are inert. This must hold in Release (-DNDEBUG) and sanitizer builds.
    constexpr int contenders = 32;
    std::atomic<bool> start_completion{false};
    std::atomic<int> completed_receipts{0};
    std::vector<std::thread> completion_observers;
    completion_observers.reserve(contenders);
    for (int i = 0; i < contenders; ++i) {
        completion_observers.emplace_back([&] {
            while (!start_completion.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (pin.release_after_ticket_retirement())
                completed_receipts.fetch_add(1, std::memory_order_relaxed);
        });
    }
    start_completion.store(true, std::memory_order_release);
    for (auto& observer : completion_observers) observer.join();
    CHECK(completed_receipts.load(std::memory_order_relaxed) == 1);
    CHECK(!pin.active());
    CHECK(ref.outstanding_resource_loans() == 0);
    // A once-live but now retired exact ticket cannot reacquire arena
    // lifetime, even before the resource registry's final close runs.
    bool retired_ticket_rejected = false;
    try {
        StateRegionActorLifetimePin replay(
            ref, *arena, region, request.ticket);
    } catch (const std::invalid_argument&) {
        retired_ticket_rejected = true;
    }
    CHECK(retired_ticket_rejected);
    CHECK(ref.outstanding_resource_loans() == 0);

    CHECK(owned.poll() == NativeActorRetirementBarrier::Poll::completed);
    CHECK(freed == 1);
    ref.get_done().get();

    // Release is linear and physical reclamation occurs exactly once.
    CHECK(!pin.release_after_ticket_retirement());
    CHECK(owned.poll() == NativeActorRetirementBarrier::Poll::completed);
    CHECK(freed == 1);

    return 0;
}
