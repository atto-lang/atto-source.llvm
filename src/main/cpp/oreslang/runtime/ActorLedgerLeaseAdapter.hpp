#pragma once

#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include "oreslang/runtime/NativeActorScheduler.h"
#include <cstddef>
#include <cstdint>

namespace oreslang::runtime {

// The ONLY permitted entry point to actor-private ledger mutations from
// trusted native scheduler callbacks. A returned lease check is deliberately
// not cached: another carrier or a later STOP cannot reuse it.
// No timer/IO/Future completion thread may call a mutation directly.
// Host boundary only; source-generated continuation/heap pins remain pending.
template<std::size_t Capacity> class ActorLedgerLeaseAdapter final {
public:
    using Ledger = ActorExecutionLedger<Capacity>;
    using Status = typename Ledger::Status;
    using Token = typename Ledger::Token;
    using Kind = typename Ledger::Kind;
    using Selection = typename Ledger::Selection;
    using Snapshot = typename Ledger::Snapshot;

    ActorLedgerLeaseAdapter(OresNativeActorScheduler* scheduler,
                            OresNativeActorRef actor,
                            std::uint64_t unique_actor_id) noexcept
        : scheduler_(scheduler), actor_(actor),
          ledger_(unique_actor_id, actor.generation) {}

    ActorLedgerLeaseAdapter(const ActorLedgerLeaseAdapter&) = delete;
    ActorLedgerLeaseAdapter& operator=(const ActorLedgerLeaseAdapter&) = delete;

    bool current_actor_lease() const noexcept {
        return scheduler_ != nullptr &&
            ores_native_actor_scheduler_is_current_actor(scheduler_, actor_) != 0;
    }

    Status admit(Kind kind, Token& out) noexcept {
        return current_actor_lease() ? ledger_.admit(kind, out) : Status::Unauthorized;
    }
    Status claim(Selection& out) noexcept {
        return current_actor_lease() ? ledger_.claim_under_lease(out) : Status::Unauthorized;
    }
    Status prepare_await(Token t) noexcept {
        return current_actor_lease() ? ledger_.prepare_await(t) : Status::Unauthorized;
    }
    Status suspend(Token t) noexcept {
        return current_actor_lease() ? ledger_.suspend(t) : Status::Unauthorized;
    }
    Status deliver_wake(Token t) noexcept {
        return current_actor_lease() ? ledger_.wake(t) : Status::Unauthorized;
    }
    Status cancel(Token t) noexcept {
        return current_actor_lease() ? ledger_.cancel(t) : Status::Unauthorized;
    }
    // Called after returning from the guest segment, while its physical actor
    // execution claim is still held by the scheduler callback wrapper.
    Status finish_after_guest_exit(Token t) noexcept {
        return current_actor_lease() ? ledger_.finish_after_exit(t) : Status::Unauthorized;
    }
    Status stop_and_request_cleanup() noexcept {
        return current_actor_lease() ? ledger_.stop_and_request_cleanup()
                                     : Status::Unauthorized;
    }
    bool snapshot(Snapshot& out) const noexcept {
        if (!current_actor_lease()) return false;
        out=ledger_.snapshot();
        return true;
    }
private:
    OresNativeActorScheduler* const scheduler_;
    const OresNativeActorRef actor_;
    Ledger ledger_;
};
}  // namespace oreslang::runtime
