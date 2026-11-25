#pragma once

#include "oreslang/runtime/ActorExecutionLedger.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime {

// Actor-owned bounded logical frame storage, independent of native carrier
// stacks. Host/scheduler-only: callers must already hold the authenticated
// actor lease and must supply a token minted by ActorExecutionLedger. This
// class does not itself authenticate native threads or guest-supplied tokens.
//
// Storage is preallocated in each actor. BytesPerStack is physical slot
// capacity; ActorBytes is a *live* aggregate quota (including suspended
// stacks), not the class's sizeof footprint.
template<std::size_t Slots, std::size_t FramesPerStack,
         std::size_t BytesPerStack, std::size_t ActorBytes>
class ActorInvocationFrames final {
    static_assert(Slots > 0 && Slots <= UINT32_MAX, "invalid invocation slots");
    static_assert(FramesPerStack > 0 && FramesPerStack <= UINT32_MAX,
                  "invalid per-invocation frame bound");
    static_assert(BytesPerStack > 0 && ActorBytes > 0,
                  "nonzero bounded actor stack capacities required");
public:
    using Ledger = ActorExecutionLedger<Slots>;
    using Token = typename Ledger::Token;
    enum class Status : std::uint8_t {
        Ok, Stale, Busy, NotRunning, NotSuspended, Invalid,
        NoFrame, OutOfBounds, QuotaExceeded, Poisoned
    };
    struct FrameRef {
        Token owner{};
        std::uint64_t nonce=0;
        std::uint32_t depth=0;
        // Physical frame-arena instance; not derived from reusable actor IDs.
        std::uint64_t instance=0;
    };
    struct Snapshot {
        std::size_t live_stacks=0;
        std::size_t live_frames=0;
        std::size_t charged_bytes=0;
        std::size_t peak_bytes=0;
        bool poisoned=false;
    };

    ActorInvocationFrames(std::uint64_t actor, std::uint64_t generation) noexcept
        : actor_(actor), generation_(generation), instance_(mintInstance()),
          poisoned_(!actor || !generation || !instance_) {}
    ActorInvocationFrames(const ActorInvocationFrames&) = delete;
    ActorInvocationFrames& operator=(const ActorInvocationFrames&) = delete;

    // Bind immediately after successful ledger admit; no guest frame exists
    // yet. Reject a recycled slot's old token even after physical retirement.
    Status bind(Token t) noexcept {
        // Authenticate actor/incarnation before revealing poison status.
        if (!validIdentity(t)) return Status::Stale;
        if (poisoned_) return Status::Poisoned;
        Slot& s=slots_[t.slot];
        // After a slot is recycled, an old token remains stale even when the
        // new incarnation is actively queued/running in the same slot.
        if (t.sequence<=s.last_sequence ||
            t.slot_generation<=s.last_slot_generation) return Status::Stale;
        if (s.phase!=Phase::Free) return Status::Busy;
        s.owner=t;
        s.phase=Phase::Queued;
        s.depth=0;
        s.used=0;
        return Status::Ok;
    }

    // Call only after ledger.claim_under_lease. Explicitly keep one running
    // stack per actor even if different scheduler carriers try to enter.
    Status enter(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (poisoned_) return Status::Poisoned;
        if (s->phase!=Phase::Queued && s->phase!=Phase::Suspended)
            return Status::Busy;
        for (const Slot& other:slots_)
            if (&other!=s && other.phase==Phase::Running) return Status::Busy;
        s->phase=Phase::Running;
        return Status::Ok;
    }

    // A terminally poisoned actor must still be able to complete bounded
    // physical STOP cleanup. This grants *no* guest frame read/write/push
    // access and is only called after the ledger selected CancelCleanup.
    Status enter_for_cleanup(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (s->phase!=Phase::Queued && s->phase!=Phase::Suspended)
            return Status::Busy;
        for (const Slot& other:slots_)
            if (&other!=s && (other.phase==Phase::Running ||
                               other.phase==Phase::Cleanup)) return Status::Busy;
        s->phase=Phase::Cleanup;
        return Status::Ok;
    }

    Status suspend(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (poisoned_) return Status::Poisoned;
        if (s->phase!=Phase::Running) return Status::NotRunning;
        s->phase=Phase::Suspended;
        return Status::Ok;
    }

    Status push(Token t, std::size_t bytes, FrameRef& out) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (poisoned_) return Status::Poisoned;
        if (s->phase!=Phase::Running) return Status::NotRunning;
        if (!bytes) return Status::Invalid;
        if (s->depth>=FramesPerStack || bytes>BytesPerStack-s->used ||
            bytes>ActorBytes-charged_ ||
            s->next_nonce==std::numeric_limits<std::uint64_t>::max()) {
            poisoned_=true; // terminal even when a guest swallows the error
            return Status::QuotaExceeded;
        }
        const std::uint64_t nonce=++s->next_nonce;
        s->frames[s->depth]={s->used,bytes,nonce};
        s->used+=bytes;
        ++s->depth;
        ++live_frames_;
        charged_+=bytes;
        if (charged_>peak_) peak_=charged_;
        out={t,nonce,static_cast<std::uint32_t>(s->depth),instance_};
        return Status::Ok;
    }

    Status write(Token t, FrameRef ref, std::size_t offset,
                 const std::uint8_t* src, std::size_t length) noexcept {
        Slot* s=nullptr;
        Frame* frame=nullptr;
        Status st=top(t,ref,s,frame);
        if (st!=Status::Ok) return st;
        if ((!src && length) || offset>frame->size || length>frame->size-offset) {
            poisoned_=true;
            return Status::OutOfBounds;
        }
        for (std::size_t i=0;i<length;++i)
            s->storage[frame->offset+offset+i]=src[i];
        return Status::Ok;
    }

    Status read(Token t, FrameRef ref, std::size_t offset,
                std::uint8_t* dst, std::size_t length) noexcept {
        Slot* s=nullptr;
        Frame* frame=nullptr;
        Status st=top(t,ref,s,frame);
        if (st!=Status::Ok) return st;
        if ((!dst && length) || offset>frame->size || length>frame->size-offset) {
            poisoned_=true;
            return Status::OutOfBounds;
        }
        for (std::size_t i=0;i<length;++i)
            dst[i]=s->storage[frame->offset+offset+i];
        return Status::Ok;
    }

    Status pop(Token t, FrameRef ref) noexcept {
        Slot* s=nullptr;
        Frame* frame=nullptr;
        Status st=top(t,ref,s,frame);
        if (st!=Status::Ok) return st;
        const std::size_t size=frame->size;
        scrub(*s,frame->offset,size);
        s->used-=size;
        *frame={};
        --s->depth;
        --live_frames_;
        charged_-=size;
        return Status::Ok;
    }

    // Call after actual guest callback exit under actor lease, before ledger
    // finish_after_exit acknowledges slot retirement. A normal return must
    // have popped every frame. Cancellation may scrub leftover frames.
    Status retire_after_guest_exit(Token t, bool cancelled=false) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (s->phase!=Phase::Running &&
            !(cancelled && s->phase==Phase::Cleanup)) return Status::NotRunning;
        if (s->depth && !cancelled) {
            poisoned_=true;
            return Status::Invalid;
        }
        retireSlot(*s);
        return Status::Ok;
    }

    // Only zero-frame queued cancellation, after ledger.cancel retired it.
    Status retire_queued(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (s->phase!=Phase::Queued || s->depth) return Status::Busy;
        retireSlot(*s);
        return Status::Ok;
    }

    Snapshot snapshot() const noexcept {
        std::size_t live=0;
        for (const Slot& s:slots_) if (s.phase!=Phase::Free) ++live;
        return {live,live_frames_,charged_,peak_,poisoned_};
    }
    bool poisoned() const noexcept { return poisoned_; }

private:
    enum class Phase : std::uint8_t { Free, Queued, Running, Suspended, Cleanup };
    struct Frame { std::size_t offset=0,size=0; std::uint64_t nonce=0; };
    struct Slot {
        Token owner{};
        Phase phase=Phase::Free;
        std::uint64_t last_sequence=0,last_slot_generation=0,next_nonce=0;
        std::array<Frame,FramesPerStack> frames{};
        std::array<std::uint8_t,BytesPerStack> storage{};
        std::size_t depth=0,used=0;
    };

    // Token sequences restart on a newly constructed actor ledger. A frame
    // handle must also bind to the particular physical frame-arena object.
    // This per-template counter is sufficient because FrameRef types from
    // different instantiations are not interchangeable. Exhaustion poisons
    // construction rather than wrapping to an earlier instance identity.
    static std::uint64_t mintInstance() noexcept {
        static std::atomic<std::uint64_t> next{1};
        std::uint64_t expected=next.load(std::memory_order_relaxed);
        while (expected!=0 && expected!=std::numeric_limits<std::uint64_t>::max()) {
            if (next.compare_exchange_weak(expected,expected+1,
                    std::memory_order_relaxed,std::memory_order_relaxed))
                return expected;
        }
        return 0;
    }

    static bool same(Token a, Token b) noexcept {
        return a.actor==b.actor && a.generation==b.generation &&
            a.sequence==b.sequence && a.slot_generation==b.slot_generation &&
            a.slot==b.slot;
    }
    bool validIdentity(Token t) const noexcept {
        return t.actor==actor_ && t.generation==generation_ &&
            t.sequence!=0 && t.slot_generation!=0 && t.slot<Slots;
    }
    Slot* find(Token t) noexcept {
        if (!validIdentity(t)) return nullptr;
        Slot& s=slots_[t.slot];
        return s.phase!=Phase::Free && same(s.owner,t) ? &s : nullptr;
    }
    Status top(Token t, FrameRef ref, Slot*& slot, Frame*& frame) noexcept {
        slot=find(t); if (!slot) return Status::Stale;
        if (poisoned_) return Status::Poisoned;
        if (slot->phase!=Phase::Running) return Status::NotRunning;
        if (!same(t,ref.owner) || !slot->depth ||
            ref.instance!=instance_) return Status::NoFrame;
        frame=&slot->frames[slot->depth-1];
        if (ref.depth!=slot->depth || ref.nonce!=frame->nonce) return Status::NoFrame;
        return Status::Ok;
    }
    static void scrub(Slot& s,std::size_t offset,std::size_t size) noexcept {
        // Volatile stores prevent ordinary dead-store deletion of the clear.
        volatile std::uint8_t* bytes=s.storage.data();
        for (std::size_t i=0;i<size;++i) bytes[offset+i]=0;
    }
    void retireSlot(Slot& s) noexcept {
        scrub(s,0,BytesPerStack);
        charged_-=s.used;
        live_frames_-=s.depth;
        s.last_sequence=s.owner.sequence;
        s.last_slot_generation=s.owner.slot_generation;
        s.owner={};
        s.depth=0; s.used=0;
        for (Frame& frame:s.frames) frame={};
        s.phase=Phase::Free;
    }

    std::array<Slot,Slots> slots_{};
    std::uint64_t actor_,generation_,instance_;
    std::size_t charged_=0,peak_=0,live_frames_=0;
    bool poisoned_=false;
};

} // namespace oreslang::runtime
