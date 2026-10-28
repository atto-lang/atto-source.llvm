#pragma once
#include "oreslang/runtime/NativeActorLedgerBridge.hpp"
#include "oreslang/runtime/NativeAwaitTraceBridge.h"
#include <cstdint>

namespace oreslang::runtime {

// Actor-private host-only binding of one logical actor event to a REAL
// NativeContinuationStore suspension record. Bootstrap must create both
// context stores before the RT-critical execution phase. No pointers to
// guest captures are ever placed in scheduler CONTROL or timer queues.
// Store closure requires an independent producer-quiescence fence.
template<typename Bridge>
class NativeActorAwaitLedgerLink final {
public:
    using Id = typename Bridge::Id;
    enum class State : std::uint8_t { Fresh, Awaiting, Resuming, Finished, Cancelled };
    enum class Wake : std::uint8_t { Accepted, RetryRequired, InvalidOrCancelled };
    struct Ticket {
        Id event{};
        OresNativeSuspensionLink link{};
        std::uint64_t code_generation = 0;
        std::uint64_t source_token = 0;
    };

    NativeActorAwaitLedgerLink(Bridge& bridge, OresNativeActorScheduler* scheduler,
                               OresNativeExecutionContextStore* executions,
                               OresNativeContinuationStore* continuations) noexcept
        : bridge_(bridge), scheduler_(scheduler),
          executions_(executions), continuations_(continuations) {}
    NativeActorAwaitLedgerLink(const NativeActorAwaitLedgerLink&) = delete;
    NativeActorAwaitLedgerLink& operator=(const NativeActorAwaitLedgerLink&) = delete;

    // Called from original guest handler before it returns Suspend. The
    // authenticated native execution context supplies the actual allocation
    // domain; neither the Future producer nor the scheduler can forge it.
    bool suspend(Id event, std::uint64_t code_generation,
                 std::uint64_t source_token) noexcept {
        if (state_ != State::Fresh || !bridge_.actor_authorized() ||
            !executions_ || !continuations_ ||
            !event.sequence || !event.actor_generation ||
            !event.scheduler_domain || !code_generation)
            return false;
        OresNativeExecutionContext execution{};
        if (!ores_native_actor_scheduler_current_execution_context(
                scheduler_, &execution) ||
            execution.id.runtime_domain != event.scheduler_domain ||
            execution.allocation.kind != ORES_ALLOCATION_DOMAIN_ACTOR ||
            execution.allocation.owner != event.actor_id - 1 ||
            !execution.allocation.generation)
            return false;
        OresNativeExecutionHandle parent{};
        if (!ores_native_execution_context_store_insert(
                executions_, &execution, &parent)) return false;
        OresNativeSuspensionLink link{};
        if (!ores_native_await_trace_suspend(
                executions_, continuations_, parent,
                OresNativeContinuationHandle{},
                ORES_CONTINUATION_BOUNDARY_AWAIT,
                code_generation, source_token, &link)) {
            (void)ores_native_execution_context_store_release(executions_, parent);
            return false;
        }
        ticket_ = Ticket{event, link, code_generation, source_token};
        state_ = State::Awaiting;
        return true;
    }

    // A *copy* of this ticket is published by the actor using an external
    // release fence before any producer receives it. The producer never
    // dereferences actor-owned ledger state or this binding object.
    bool ticket_for_actor(Ticket& out) const noexcept {
        if (!bridge_.actor_authorized() || state_ != State::Awaiting) return false;
        out = ticket_;
        return true;
    }

    // Producer-side: NativeContinuationStore serializes READY vs CANCELLED.
    // If scheduler ingress is saturated, the Future remains READY and the
    // producer gets a typed retry requirement, not a fabricated completion.
    static Wake publish_ready(OresNativeContinuationStore* continuations,
                              Bridge& bridge, Ticket ticket) noexcept {
        if (!continuations || !ticket.event.sequence ||
            !ores_native_continuation_store_notify_ready(
                continuations, ticket.link.continuation)) return Wake::InvalidOrCancelled;
        return forward_ready(bridge, ticket);
    }

    // Repeat only after READY was genuinely published; no re-publication
    // into the store, since duplicate READY loses its state transition.
    static Wake forward_ready(Bridge& bridge, Ticket ticket) noexcept {
        const auto status = bridge.notify_ready(ticket.event);
        using Notify = typename Bridge::Notify;
        if (status == Notify::Signaled || status == Notify::Coalesced ||
            status == Notify::RetryRequired) return Wake::Accepted;
        if (status == Notify::Full) return Wake::RetryRequired;
        return Wake::InvalidOrCancelled;
    }

    // Actor-carrier-only: called when the *same* event was selected by the
    // actor ledger. NativeAwaitTraceBridge requires READY; it competes
    // atomically against cancellation and creates a new execution trace.
    bool resume_begin(Id selected, OresNativeExecutionContext* out) noexcept {
        if (!bridge_.actor_authorized() || state_ != State::Awaiting ||
            !same(selected, ticket_.event)) return false;
        OresNativeExecutionContext current{};
        if (!ores_native_actor_scheduler_current_execution_context(
                scheduler_, &current)) return false;
        OresNativeExecutionContext resumed{};
        OresNativeExecutionHandle execution{};
        if (!ores_native_await_trace_ready_resume_begin(
                executions_, continuations_, current.id, ticket_.link,
                &execution, &resumed)) return false;
        if (resumed.allocation.domain == 0 ||
            resumed.allocation.owner != selected.actor_id - 1 ||
            resumed.allocation.generation == 0) return false; // quarantine on mismatch
        resumed_ = execution;
        state_ = State::Resuming;
        if (out) *out = resumed;
        return true;
    }

    // Must be called by the enclosing native carrier *after* guest handler
    // code returns; not from inside the resumed guest function body. An
    // actor's ledger/heap cannot be reclaimed while its native callback runs.
    bool finish_after_guest_return() noexcept {
        if (!bridge_.actor_authorized() || state_ != State::Resuming) return false;
        if (!ores_native_await_trace_resume_finish(continuations_, ticket_.link))
            return false;
        const bool ok = ores_native_execution_context_store_release(
                executions_, resumed_) != 0 &&
            ores_native_continuation_store_release(
                continuations_, ticket_.link.continuation) != 0 &&
            ores_native_execution_context_store_release(
                executions_, ticket_.link.parent) != 0;
        if (ok) state_ = State::Finished;
        return ok;
    }

    // Actor-control-only cancellation. Native continuation cancellation must
    // WIN before discarding the matching logical event. A claimed resume
    // cannot be cancelled and remains pinned through physical return.
    bool cancel_under_actor_lease() noexcept {
        if (!bridge_.actor_authorized() || state_ != State::Awaiting)
            return false;
        if (!ores_native_continuation_store_cancel(
                continuations_, ticket_.link.continuation)) return false;
        if (bridge_.cancel_event(ticket_.event) != Bridge::Status::Ok)
            return false; // fail closed: requires supervision/quarantine
        const bool ok = ores_native_continuation_store_release(
                continuations_, ticket_.link.continuation) != 0 &&
            ores_native_execution_context_store_release(
                executions_, ticket_.link.parent) != 0;
        if (ok) state_ = State::Cancelled;
        return ok;
    }

    State state_for_actor() const noexcept { return state_; } // owner-carrier only

private:
    static bool same(Id a, Id b) noexcept {
        return a.actor_id == b.actor_id &&
            a.actor_generation == b.actor_generation &&
            a.scheduler_domain == b.scheduler_domain &&
            a.sequence == b.sequence;
    }
    Bridge& bridge_;
    OresNativeActorScheduler* scheduler_;
    OresNativeExecutionContextStore* executions_;
    OresNativeContinuationStore* continuations_;
    Ticket ticket_{};
    OresNativeExecutionHandle resumed_{};
    State state_ = State::Fresh;
};
} // namespace oreslang::runtime
