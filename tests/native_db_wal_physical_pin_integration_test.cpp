#include "oreslang/runtime/DbWalPhysicalRetirement.hpp"
#include "oreslang/runtime/NativeActorRetirementBarrier.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>

using namespace oreslang::runtime;

int main() {
    constexpr std::uint64_t actor_id = 57;
    constexpr std::uint64_t generation = 9;
    NativeActorResourceQuarantine quarantine(4);
    NativeActorGroup group(770, 1);
    auto registry = std::make_shared<ActorResourceRegistry>(actor_id, generation);

    NativeActorRetirementBarrier* barrier = nullptr;
    auto ref = group.adopt(770, actor_id, generation,
            [&] { barrier->retire(); });
    NativeActorRetirementBarrier owned(
            registry, group.runtime_finalizer(ref), quarantine);
    barrier = &owned;
    assert(group.mark_ready(ref));

    DbWalOwnerRetirementGate wal(actor_id,
            static_cast<std::uint32_t>(generation));
    assert(wal.admit(actor_id, static_cast<std::uint32_t>(generation)));
    const AsyncIoTicket ticket{actor_id, generation, 73};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoActorLifetimePin pin(ref, lifecycle);
    DbWalPhysicalRetirement physical(wal, lifecycle, pin);

    assert(lifecycle.mark_registered(ticket));
    assert(lifecycle.begin_callback(ticket));
    assert(!wal.request_drain(actor_id,
            static_cast<std::uint32_t>(generation)));
    assert(!wal.begin_close(actor_id,
            static_cast<std::uint32_t>(generation)));
    group.close();
    assert(owned.poll() == NativeActorRetirementBarrier::Poll::not_admitted);
    assert(ref.get_done().wait_for(std::chrono::milliseconds(0))
            == std::future_status::timeout);

    // Logically cancelled I/O may still have a native callback or kernel
    // registration: neither the WAL file nor actor heap may be released.
    assert(lifecycle.cancel(ticket));
    assert(!physical.retire());
    assert(wal.active());
    assert(pin.active());
    assert(!wal.begin_close(actor_id,
            static_cast<std::uint32_t>(generation)));
    assert(owned.poll() == NativeActorRetirementBarrier::Poll::not_admitted);

    assert(lifecycle.end_callback(ticket));
    assert(!physical.retire());
    assert(lifecycle.mark_unregistered(ticket));
    assert(!physical.retire());
    assert(lifecycle.retire(ticket));
    assert(physical.retire());
    assert(physical.retired());
    assert(!physical.retire());
    assert(!pin.active());
    assert(!wal.active());
    assert(ref.outstanding_resource_loans() == 0);

    assert(wal.request_drain(actor_id,
            static_cast<std::uint32_t>(generation)));
    assert(!wal.begin_close(actor_id + 1,
            static_cast<std::uint32_t>(generation)));
    assert(wal.begin_close(actor_id,
            static_cast<std::uint32_t>(generation)));
    wal.physical_close_retired(actor_id,
            static_cast<std::uint32_t>(generation));
    assert(owned.poll() == NativeActorRetirementBarrier::Poll::completed);
    assert(ref.get_done().wait_for(std::chrono::milliseconds(0))
            == std::future_status::ready);
    ref.get_done().get();
    return 0;
}
