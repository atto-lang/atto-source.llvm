#pragma once

#include "oreslang/runtime/ActorLedgerLeaseAdapter.hpp"
#include "oreslang/runtime/ActorInvocationFrames.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace oreslang::runtime {

/**
 * Lease-authenticated, actor-incarnation-owned frame/continuation coordinator.
 *
 * Exactly one trusted scheduler callback may mutate either ledger or frame
 * store. Neither ledger tokens nor guest-supplied frame handles are authority:
 * the physical actor lease is checked for EVERY operation. This coordinates
 * the existing serial DATA/WAKE ledger with bounded, separate per-invocation
 * stack storage, not OS-native stack switching or .atto source CALL lowering.
 */
template<std::size_t Capacity, std::size_t Depth,
         std::size_t StackBytes, std::size_t ActorBytes>
class ActorInvocationFrameBridge final {
public:
    using Adapter=ActorLedgerLeaseAdapter<Capacity>;
    using Ledger=typename Adapter::Ledger;
    using Status=typename Adapter::Status;
    using Kind=typename Adapter::Kind;
    using Token=typename Adapter::Token;
    using Selection=typename Adapter::Selection;
    using Frames=ActorInvocationFrames<Capacity,Depth,StackBytes,ActorBytes>;
    using FrameStatus=typename Frames::Status;
    using FrameRef=typename Frames::FrameRef;
    struct FrameResult {
        Status admission=Status::Unauthorized;
        FrameStatus frame=FrameStatus::Stale;
        bool ok() const noexcept {
            return admission==Status::Ok && frame==FrameStatus::Ok;
        }
    };

    ActorInvocationFrameBridge(OresNativeActorScheduler* scheduler,
                               OresNativeActorRef actor,
                               std::uint64_t unique_actor_id) noexcept
        : ledger_(scheduler,actor,unique_actor_id),
          frames_(unique_actor_id,actor.generation) {}
    ActorInvocationFrameBridge(const ActorInvocationFrameBridge&)=delete;
    ActorInvocationFrameBridge& operator=(const ActorInvocationFrameBridge&)=delete;

    bool has_actor_lease() const noexcept { return ledger_.current_actor_lease(); }

    Status admit(Kind kind,Token& out) noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        if (faulted_ || frames_.poisoned()) return Status::Invalid;
        Token token{};
        const Status admitted=ledger_.admit(kind,token);
        if (admitted!=Status::Ok) return admitted;
        const FrameStatus reserved=frames_.bind(token);
        if (reserved!=FrameStatus::Ok) {
            // Ledger admission is still queued here. Cancel it before
            // returning, so no unframed guest callback can execute.
            const Status cancelled=ledger_.cancel(token);
            faulted_=true;
            if (cancelled!=Status::Ok) return Status::Invalid;
            return Status::Invalid;
        }
        bound_[token.slot]=token;
        cancel_requested_[token.slot]=false;
        out=token;
        return Status::Ok;
    }

    Status claim(Selection& out) noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        Selection selected{};
        const Status s=ledger_.claim(selected);
        if (s!=Status::Ok) return s;
        // A poisoned actor can execute only explicit STOP cleanup turns,
        // never new or resumed guest DATA.
        const bool cleanup=selected.dispatch==Ledger::Dispatch::CancelCleanup;
        const FrameStatus f=cleanup
            ? frames_.enter_for_cleanup(selected.token)
            : (faulted_ || frames_.poisoned()
                ? FrameStatus::Poisoned : frames_.enter(selected.token));
        if (f!=FrameStatus::Ok) {
            faulted_=true;
            return Status::Invalid;
        }
        out=selected;
        return Status::Ok;
    }

    Status prepare_await(Token token) noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        if (faulted_ || frames_.poisoned()) return Status::Invalid;
        return ledger_.prepare_await(token);
    }

    Status suspend(Token token) noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        if (faulted_ || frames_.poisoned()) return Status::Invalid;
        const FrameStatus f=frames_.suspend(token);
        if (f!=FrameStatus::Ok) return Status::Invalid;
        const Status l=ledger_.suspend(token);
        if (l==Status::Ok) return l;
        // A completed future may win registration->suspend race. Restore the
        // very same stack and execute inline; do not park the carrier.
        const FrameStatus restored=frames_.enter(token);
        if (restored!=FrameStatus::Ok) {
            faulted_=true;
            return Status::Invalid;
        }
        return l;
    }

    Status deliver_wake(Token token) noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        return ledger_.deliver_wake(token);
    }

    FrameResult push(Token t,std::size_t bytes,FrameRef& out) noexcept {
        if (!has_actor_lease()) return {};
        const FrameStatus f=frames_.push(t,bytes,out);
        if (frames_.poisoned()) faulted_=true;
        return {Status::Ok,f};
    }
    FrameResult read(Token t,FrameRef ref,std::size_t offset,
                     std::uint8_t* bytes,std::size_t length) noexcept {
        if (!has_actor_lease()) return {};
        const FrameStatus f=frames_.read(t,ref,offset,bytes,length);
        if (frames_.poisoned()) faulted_=true;
        return {Status::Ok,f};
    }
    FrameResult write(Token t,FrameRef ref,std::size_t offset,
                      const std::uint8_t* bytes,std::size_t length) noexcept {
        if (!has_actor_lease()) return {};
        const FrameStatus f=frames_.write(t,ref,offset,bytes,length);
        if (frames_.poisoned()) faulted_=true;
        return {Status::Ok,f};
    }
    FrameResult pop(Token t,FrameRef ref) noexcept {
        if (!has_actor_lease()) return {};
        const FrameStatus f=frames_.pop(t,ref);
        if (frames_.poisoned()) faulted_=true;
        return {Status::Ok,f};
    }

    Status cancel(Token t) noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        const Status l=ledger_.cancel(t);
        if (l!=Status::Ok) return l;
        // Queued cancellation is immediately physically retired by ledger.
        // A running or suspended token remains framed until guest exit.
        cancel_requested_[t.slot]=true;
        if (frames_.retire_queued(t)==FrameStatus::Ok) {
            bound_[t.slot]={};
            cancel_requested_[t.slot]=false;
        }
        return l;
    }

    Status finish_after_guest_exit(Token t,bool cancelled=false) noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        // On terminal poison we permit only STOP/cancellation cleanup. Never
        // interpret a swallowed quota error as an ordinary successful RET.
        if (cancelled && (t.slot>=Capacity ||
                !cancel_requested_[t.slot] ||
                bound_[t.slot].sequence!=t.sequence ||
                bound_[t.slot].slot_generation!=t.slot_generation)) {
            return Status::Invalid; // cleanup requires proven STOP/cancel intent
        }
        if ((faulted_ || frames_.poisoned()) && !cancelled) return Status::Invalid;
        const FrameStatus f=frames_.retire_after_guest_exit(t,cancelled);
        if (f!=FrameStatus::Ok) {
            faulted_=true;
            return Status::Invalid;
        }
        const Status l=ledger_.finish_after_guest_exit(t);
        if (l!=Status::Ok) {
            faulted_=true;
            return l;
        }
        bound_[t.slot]={};
        cancel_requested_[t.slot]=false;
        return Status::Ok;
    }

    Status stop_and_request_cleanup() noexcept {
        if (!has_actor_lease()) return Status::Unauthorized;
        const Status l=ledger_.stop_and_request_cleanup();
        if (l!=Status::Ok) return l;
        for (std::size_t i=0;i<Capacity;++i) {
            Token t=bound_[i];
            if (!t.sequence) continue;
            cancel_requested_[i]=true;
            if (frames_.retire_queued(t)==FrameStatus::Ok) {
                bound_[i]={};
                cancel_requested_[i]=false;
            }
        }
        return l;
    }

    bool snapshot(typename Ledger::Snapshot& ledger,
                  typename Frames::Snapshot& frames) const noexcept {
        if (!has_actor_lease()) return false;
        if (!ledger_.snapshot(ledger)) return false;
        frames=frames_.snapshot();
        return true;
    }

private:
    Adapter ledger_;
    Frames frames_;
    std::array<Token,Capacity> bound_{};
    std::array<bool,Capacity> cancel_requested_{};
    bool faulted_=false;
};

} // namespace oreslang::runtime
