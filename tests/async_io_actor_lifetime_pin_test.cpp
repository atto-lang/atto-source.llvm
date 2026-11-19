#include "oreslang/runtime/AsyncIoActorLifetimePin.hpp"
#include "oreslang/runtime/NativeActorRetirementBarrier.hpp"

#include <cassert>
#include <chrono>
#include <memory>

using namespace oreslang::runtime;

int main() {
    NativeActorResourceQuarantine quarantine(4);
    NativeActorGroup group(611, 1);

    constexpr std::uint64_t actor_id = 41;
    constexpr std::uint64_t generation = 7;
    auto registry = std::make_shared<ActorResourceRegistry>(
        actor_id, generation);

    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(
        611, actor_id, generation, [&] { barrier->retire(); });
    NativeActorRetirementBarrier owned(
        registry, group.runtime_finalizer(ref), quarantine);
    barrier = &owned;

    assert(group.mark_ready(ref));

    const AsyncIoTicket ticket{actor_id, generation, 9001};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoActorLifetimePin pin(ref, lifecycle);
    assert(pin.active());
    assert(ref.outstanding_resource_loans() == 1);

    // Retirement request detaches the group, but the I/O loan prevents the
    // runtime finalizer from reclaiming resources or settling get_done().
    group.close();
    assert(owned.poll() == NativeActorRetirementBarrier::Poll::not_admitted);
    assert(ref.get_done().wait_for(std::chrono::milliseconds(0))
        == std::future_status::timeout);

    // Logical Future settlement alone is insufficient.
    assert(lifecycle.mark_registered(ticket));
    assert(lifecycle.begin_callback(ticket));
    assert(lifecycle.finish(ticket, AsyncIoTerminal::success));
    assert(!pin.release_after_physical_retirement());
    assert(ref.outstanding_resource_loans() == 1);
    assert(owned.poll() == NativeActorRetirementBarrier::Poll::not_admitted);
    assert(ref.get_done().wait_for(std::chrono::milliseconds(0))
        == std::future_status::timeout);

    // Even after the callback returns, the kernel registration still pins done.
    assert(lifecycle.end_callback(ticket));
    assert(!pin.release_after_physical_retirement());
    assert(lifecycle.mark_unregistered(ticket));
    assert(!pin.release_after_physical_retirement());

    // Only explicit physical retirement releases the actor incarnation loan.
    assert(lifecycle.retire(ticket));
    assert(lifecycle.ready_to_reclaim());
    assert(pin.release_after_physical_retirement());
    assert(!pin.active());
    assert(ref.outstanding_resource_loans() == 0);

    assert(owned.poll() == NativeActorRetirementBarrier::Poll::completed);
    assert(ref.get_done().wait_for(std::chrono::milliseconds(0))
        == std::future_status::ready);
    ref.get_done().get();

    // A different actor generation can never borrow this operation lifetime.
    NativeActorGroup stale_group(612, 1);
    auto stale = stale_group.adopt(
        612, actor_id, generation + 1, [] {});
    auto stale_finalizer = stale_group.runtime_finalizer(stale);
    assert(stale_group.mark_ready(stale));
    bool rejected = false;
    try {
        AsyncIoActorLifetimePin wrong(stale, lifecycle);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    stale_group.close();
    // No lifetime pin was admitted on the mismatched generation.
    assert(stale.outstanding_resource_loans() == 0);
    assert(stale_finalizer.ready_for_resource_reclamation());
    assert(stale_finalizer.finalized());
    stale.get_done().get();

    return 0;
}
