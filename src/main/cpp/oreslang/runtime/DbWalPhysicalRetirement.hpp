#pragma once

#include "oreslang/runtime/AsyncIoActorLifetimePin.hpp"
#include "oreslang/runtime/DbWalOwnerRetirementGate.hpp"

#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace oreslang::runtime {

// Host-only bridge between the DB WAL owner gate and the existing
// NativeActorRef/AsyncIoOperationLifecycle physical retirement barrier.
// A cancellation/timeout is NOT a reclamation event; no guest ACK is emitted.
//
// The gate and pin must be acquired for the same authenticated operation;
// only its trusted physical completion owner may invoke retire(). Pin and
// lifecycle objects must outlive the bridge. No owned FDs are transferred.
class DbWalPhysicalRetirement final {
public:
    DbWalPhysicalRetirement(DbWalOwnerRetirementGate& gate,
            AsyncIoOperationLifecycle& lifecycle,
            AsyncIoActorLifetimePin& pin)
        : gate_(gate), lifecycle_(lifecycle), pin_(pin) {
        const auto& id = lifecycle_.ticket();
        if (id.generation > std::numeric_limits<std::uint32_t>::max()
                || !gate_.owns(id.actor_id,
                        static_cast<std::uint32_t>(id.generation))
                || !gate_.active())
            throw std::invalid_argument(
                "DB WAL physical pin does not match active owner incarnation");
    }

    DbWalPhysicalRetirement(const DbWalPhysicalRetirement&) = delete;
    DbWalPhysicalRetirement& operator=(const DbWalPhysicalRetirement&) = delete;

    bool retire() {
        std::lock_guard<std::mutex> hold(mu_);
        if (retired_ || !lifecycle_.ready_to_reclaim()) return false;
        // This releases the physical ActorRef resource loan. A terminal
        // Future alone cannot make this return true.
        if (!pin_.release_after_physical_retirement()) return false;
        const auto& id = lifecycle_.ticket();
        gate_.physical_write_retired(id.actor_id,
                static_cast<std::uint32_t>(id.generation));
        retired_ = true;
        return true;
    }

    bool retired() const {
        std::lock_guard<std::mutex> hold(mu_);
        return retired_;
    }
private:
    DbWalOwnerRetirementGate& gate_;
    AsyncIoOperationLifecycle& lifecycle_;
    AsyncIoActorLifetimePin& pin_;
    mutable std::mutex mu_;
    bool retired_ = false;
};

} // namespace oreslang::runtime
