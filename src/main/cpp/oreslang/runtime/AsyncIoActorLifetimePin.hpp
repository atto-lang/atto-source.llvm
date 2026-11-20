#pragma once

#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/AsyncIoOperationLifecycle.hpp"

#include <stdexcept>
#include <utility>

namespace oreslang::runtime {

// Pins one exact native actor incarnation while an admitted async-I/O operation
// may still own a kernel registration, callback, buffer or continuation edge.
//
// NativeActorRef's generation-fenced resource loan is already part of
// RuntimeFinalizer::ready_for_resource_reclamation(), so ActorGroup get_done()
// cannot settle while this pin is active.
//
// Destruction deliberately does NOT release an active pin. Losing the wrapper
// before physical I/O retirement must fail closed by leaving the actor pinned;
// silently decrementing here would permit premature arena reclamation.
class AsyncIoActorLifetimePin final {
public:
    AsyncIoActorLifetimePin(
            NativeActorRef actor,
            AsyncIoOperationLifecycle& lifecycle)
        : actor_(std::move(actor)), lifecycle_(lifecycle) {
        const auto& ticket = lifecycle_.ticket();
        if (actor_.actor_id() != ticket.actor_id
                || actor_.generation() != ticket.generation)
            throw std::invalid_argument(
                "async I/O actor lifetime pin identity mismatch");
        actor_.retain_resource_loan();
        active_ = true;
    }

    AsyncIoActorLifetimePin(const AsyncIoActorLifetimePin&) = delete;
    AsyncIoActorLifetimePin& operator=(const AsyncIoActorLifetimePin&) = delete;
    AsyncIoActorLifetimePin(AsyncIoActorLifetimePin&&) = delete;
    AsyncIoActorLifetimePin& operator=(AsyncIoActorLifetimePin&&) = delete;

    ~AsyncIoActorLifetimePin() noexcept = default;

    bool release_after_physical_retirement() noexcept {
        if (!active_ || !lifecycle_.ready_to_reclaim()) return false;
        actor_.release_resource_loan();
        active_ = false;
        return true;
    }

    bool active() const noexcept { return active_; }

private:
    NativeActorRef actor_;
    AsyncIoOperationLifecycle& lifecycle_;
    bool active_ = false;
};

} // namespace oreslang::runtime
