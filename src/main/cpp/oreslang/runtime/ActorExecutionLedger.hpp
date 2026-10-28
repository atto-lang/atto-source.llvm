#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::runtime {
// Actor-owned, single-writer, fixed-capacity continuation accounting.
// Scheduler wakeups are hints, NEVER permission to execute guest code.
// Host-only prototype: not thread safe; caller must hold a valid actor lease.
template<std::size_t N> class ActorExecutionLedger final {
    static_assert(N > 0 && N <= UINT32_MAX, "invalid ledger capacity");
public:
    enum class Status : std::uint8_t { Ok, Full, Stale, Busy, NoEligible,
        NotRunning, NotSuspended, AwaitNotArmed, AlreadyReady, AlreadyCancelled, Cancelled,
        Closed, Invalid, Overflow };
    enum class Kind : std::uint8_t { Mail, Timer, Deferred };
    enum class Dispatch : std::uint8_t { Normal, CancelCleanup };
    struct Token { std::uint64_t actor=0, generation=0, sequence=0, slot_generation=0;
        std::uint32_t slot=0; };
    struct Selection { Token token{}; Kind kind=Kind::Mail; Dispatch dispatch=Dispatch::Normal; };
    struct Snapshot { std::uint64_t revision; std::size_t live;
        bool runnable, executing, suspended, closed; };
    ActorExecutionLedger(std::uint64_t actor, std::uint64_t generation) noexcept
        : actor_(actor), generation_(generation), closed_(!actor || !generation) {}
    ActorExecutionLedger(const ActorExecutionLedger&) = delete;
    ActorExecutionLedger& operator=(const ActorExecutionLedger&) = delete;

    Status admit(Kind kind, Token& out) noexcept {
        if (closed_) return Status::Closed;
        if (next_ == UINT64_MAX || revision_ == UINT64_MAX) return Status::Overflow;
        for (std::size_t i=0; i<N; ++i) {
            Slot& s=slots_[i];
            if (s.phase!=Phase::Free || s.generation==UINT64_MAX) continue;
            ++s.generation; s.sequence=next_++; s.kind=kind;
            s.phase=Phase::Queued; s.cancel=false; s.await_armed=false;
            s.early_ready=false; ++live_;
            out=token(i); bump(); return Status::Ok;
        }
        return Status::Full;
    }
    // Register the awaiting continuation BEFORE exposing completion callbacks.
    // If completion is delivered between registration and physical suspension,
    // suspend() must not drop it or incorrectly park the actor.
    Status prepare_await(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (active_!=t.slot || s->phase!=Phase::Running) return Status::NotRunning;
        if (s->cancel) return Status::Cancelled;
        if (s->await_armed) return Status::AlreadyReady;
        s->await_armed=true; s->early_ready=false; bump(); return Status::Ok;
    }
    Status wake(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (s->phase==Phase::Ready) return Status::AlreadyReady;
        if (s->phase==Phase::Cleanup || s->cancel) return Status::AlreadyCancelled;
        if (s->phase==Phase::Running) {
            // One-writer callback-delivery seam, already holding actor lease.
            if (active_!=t.slot || !s->await_armed) return Status::NotSuspended;
            if (s->early_ready) return Status::AlreadyReady;
            s->early_ready=true; bump(); return Status::Ok;
        }
        if (s->phase!=Phase::Suspended) return Status::NotSuspended;
        s->phase=Phase::Ready; bump(); return Status::Ok;
    }
    // Invoke after acquiring physical actor execution authority; selects the
    // *same* suspended handler before any later user mail/timer callback.
    Status claim_under_lease(Selection& out) noexcept {
        if (active_!=N) return Status::Busy;
        std::size_t chosen=N;
        if (suspended_!=N) {
            Phase p=slots_[suspended_].phase;
            if (p!=Phase::Ready && p!=Phase::Cleanup) return Status::NoEligible;
            chosen=suspended_;
        } else {
            for (std::size_t i=0;i<N;++i)
                if (slots_[i].phase==Phase::Queued &&
                    (chosen==N || slots_[i].sequence<slots_[chosen].sequence)) chosen=i;
        }
        if (chosen==N) return Status::NoEligible;
        Slot& s=slots_[chosen];
        out={token(chosen),s.kind,s.phase==Phase::Cleanup?Dispatch::CancelCleanup:Dispatch::Normal};
        s.phase=Phase::Running; active_=chosen;
        if (suspended_==chosen) suspended_=N;
        bump(); return Status::Ok;
    }
    Status suspend(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (active_!=t.slot || s->phase!=Phase::Running) return Status::NotRunning;
        if (s->cancel) return Status::Cancelled;
        if (!s->await_armed) return Status::AwaitNotArmed;
        if (suspended_!=N) return Status::Busy;
        s->await_armed=false;
        if (s->early_ready) {
            // Future already completed: keep the same execution lease and
            // continue inline; callers MUST NOT park or release the carrier.
            s->early_ready=false; bump(); return Status::AlreadyReady;
        }
        s->phase=Phase::Suspended; suspended_=t.slot; active_=N;
        bump(); return Status::Ok;
    }
    // Queued events can retire immediately. Suspended events require a separate
    // actor-lease cleanup turn. Running events retain their physical lifetime.
    Status cancel(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (s->cancel || s->phase==Phase::Cleanup) return Status::AlreadyCancelled;
        if (s->phase==Phase::Queued) retire(*s);
        else if (s->phase==Phase::Running) s->cancel=true;
        else if (s->phase==Phase::Suspended || s->phase==Phase::Ready) {
            s->phase=Phase::Cleanup; s->cancel=true;
        } else return Status::Stale;
        bump(); return Status::Ok;
    }
    // Physical callback/cleanup must have exited the guest boundary BEFORE
    // this acknowledges retirement; logical cancellation is insufficient.
    Status finish_after_exit(Token t) noexcept {
        Slot* s=find(t); if (!s) return Status::Stale;
        if (active_!=t.slot || s->phase!=Phase::Running) return Status::NotRunning;
        active_=N; retire(*s); bump(); return Status::Ok;
    }
    Status stop_and_request_cleanup() noexcept {
        if (closed_) return Status::Closed;
        closed_=true;
        for (Slot& s:slots_) {
            if (s.phase==Phase::Queued) retire(s);
            else if (s.phase==Phase::Running) s.cancel=true;
            else if (s.phase==Phase::Suspended || s.phase==Phase::Ready) {
                s.phase=Phase::Cleanup; s.cancel=true;
            }
        }
        bump(); return Status::Ok;
    }
    Snapshot snapshot() const noexcept {
        bool ready=false;
        if (active_==N) {
            if (suspended_!=N) {
                Phase p=slots_[suspended_].phase;
                ready=p==Phase::Ready || p==Phase::Cleanup;
            } else for (const Slot& s:slots_) if (s.phase==Phase::Queued) {ready=true;break;}
        }
        return {revision_,live_,ready,active_!=N,suspended_!=N,closed_};
    }
    bool physically_retired() const noexcept {
        return closed_ && !live_ && active_==N && suspended_==N;
    }
private:
    enum class Phase : std::uint8_t { Free, Queued, Running, Suspended, Ready, Cleanup };
    struct Slot { std::uint64_t generation=0,sequence=0; Kind kind=Kind::Mail;
        Phase phase=Phase::Free; bool cancel=false,await_armed=false,early_ready=false; };
    Token token(std::size_t i) const noexcept {
        return {actor_,generation_,slots_[i].sequence,slots_[i].generation,
            static_cast<std::uint32_t>(i)};
    }
    Slot* find(Token t) noexcept {
        if (t.actor!=actor_ || t.generation!=generation_ || t.slot>=N ||
            !t.sequence || !t.slot_generation) return nullptr;
        Slot& s=slots_[t.slot];
        return s.phase!=Phase::Free && s.sequence==t.sequence &&
            s.generation==t.slot_generation ? &s : nullptr;
    }
    void retire(Slot& s) noexcept { s.phase=Phase::Free; s.cancel=false;
        s.await_armed=false; s.early_ready=false; --live_; }
    void bump() noexcept { if (revision_!=UINT64_MAX) ++revision_; }
    std::array<Slot,N> slots_{};
    const std::uint64_t actor_,generation_;
    std::uint64_t next_=1, revision_=0;
    std::size_t live_=0,active_=N,suspended_=N;
    bool closed_=false;
};
} // namespace oreslang::runtime
