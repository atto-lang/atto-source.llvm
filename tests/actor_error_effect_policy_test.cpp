#include "oreslang/runtime/ActorErrorCatalog.hpp"
#include "oreslang/runtime/ActorFailureControl.hpp"
#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <stdexcept>
#include <string>

using namespace oreslang::runtime;

static_assert(failure_priority(FailureKind::HostFatal) >
              failure_priority(FailureKind::Panic));
static_assert(failure_priority(FailureKind::Cancelled) >
              failure_priority(FailureKind::Panic));
static_assert(failure_priority(FailureKind::Panic) >
              failure_priority(FailureKind::Raise));
static_assert(failure_priority(FailureKind::Raise) >
              failure_priority(FailureKind::Throw));

static_assert(admits_guest_failure(
        GuestFailureBoundary::Trap, GuestFailureOrigin::OrdinaryThrow));
static_assert(!admits_guest_failure(
        GuestFailureBoundary::Trap, GuestFailureOrigin::PermissionDenied));
static_assert(!admits_guest_failure(
        GuestFailureBoundary::Trap, GuestFailureOrigin::Cancellation));
static_assert(!admits_guest_failure(
        GuestFailureBoundary::Trap, GuestFailureOrigin::Raise));
static_assert(!admits_guest_failure(
        GuestFailureBoundary::Trap, GuestFailureOrigin::Panic));
static_assert(!admits_guest_failure(
        GuestFailureBoundary::ExplicitCatch, GuestFailureOrigin::SchedulerRejected));
static_assert(!admits_guest_failure(
        GuestFailureBoundary::ExplicitCatch, GuestFailureOrigin::ProcessExit));
static_assert(!admits_guest_failure(
        GuestFailureBoundary::ExplicitCatch, GuestFailureOrigin::HostFatal));

int main() {
    // Higher-priority supervisor failures must win over ordinary throw.
    const auto ordinary = Failure::make(FailureKind::Throw, "guest throw");
    const auto terminal = Failure::make(FailureKind::Panic, "supervisor escalation");
    const auto combined = combine_failure(ordinary, terminal);
    if (combined.kind() != FailureKind::Panic) return 1;
    if (combined.suppressed().size() != 1) return 2;
    if (combined.suppressed()[0].kind() != FailureKind::Throw) return 3;

    const auto cancellation = Failure::make(FailureKind::Cancelled, "cancel");
    const auto after_cancel = combine_failure(combined, cancellation);
    if (after_cancel.kind() != FailureKind::Cancelled) return 4;
    if (after_cancel.suppressed().size() != 1) return 5;

    // An arbitrary enum tag is never admitted to CONTROL.
    bool rejected = false;
    try {
        (void)ActorErrorEnvelope::make(
            static_cast<ActorErrorCode>(65535), "unsafe", "actor", "supervisor");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) return 6;

    // Recoverable actor statuses cannot suppress cancellation.
    const auto mailbox = ActorErrorEnvelope::make(
            ActorErrorCode::ActorMailboxFull, "mailbox full", "worker", "parent");
    const auto cancel = ActorErrorEnvelope::make(
            ActorErrorCode::ActorCancelled, "cancelled", "worker", "parent");
    if (!mailbox.recoverable() || cancel.recoverable()) return 7;

    const auto raised = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-1", "parent-1");
    const auto panicked = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Panic, "actor-1", "parent-1");
    if (raised.envelope().code() != ActorErrorCode::ActorSupervision) return 8;
    if (panicked.envelope().code() != ActorErrorCode::ActorSupervision) return 9;
    if (raised.envelope().diagnostic() != "actor raised a typed Error") return 10;
    if (panicked.envelope().diagnostic() != "actor panicked with a typed Error") return 11;
    if (raised.envelope().actor_id() != "actor-1"
            || raised.envelope().supervisor_id() != "parent-1") return 12;
    if (panicked.effect() != ActorExceptionalEffect::Panic) return 13;

    try {
        (void)ActorFailureControlNotice::from_guest_effect(
                static_cast<ActorExceptionalEffect>(255), "actor-1", "parent-1");
        return 14;
    } catch (const std::invalid_argument&) {
        // Invalid effect tags must fail closed before publication.
    }
    try {
        (void)ActorFailureControlNotice::from_guest_effect(
                ActorExceptionalEffect::Raise, "", "parent-1");
        return 15;
    } catch (const std::invalid_argument&) {
        // Generic envelopes permit empty IDs; actor terminal notices do not.
    }
    try {
        (void)ActorFailureControlNotice::from_guest_effect(
                ActorExceptionalEffect::Panic, "actor-1", "unsafe/parent");
        return 16;
    } catch (const std::invalid_argument&) {
        // Supervisor identity must pass the same strict validation.
    }
    const auto root = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-1", "");
    if (!root.envelope().supervisor_id().empty()) return 17;

    // Host-owned per-supervisor queues must not route one child into another
    // supervisor's CONTROL inbox, even when the actor identity matches.
    ActorFailureControlInbox inbox("parent-1", 1);
    if (inbox.pending() != 0 || inbox.dropped() != 0) return 18;
    if (!inbox.offer(raised)) return 19;
    if (inbox.pending() != 1) return 20;
    if (inbox.offer(panicked)) return 21;
    if (inbox.dropped() != 1) return 22;
    const auto received = inbox.try_pop();
    if (!received || received->effect() != ActorExceptionalEffect::Raise) return 23;
    if (received->envelope().actor_id() != "actor-1") return 24;
    if (inbox.try_pop()) return 25;

    const auto foreign_child = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-2", "parent-2");
    try {
        (void)inbox.offer(foreign_child);
        return 26;
    } catch (const std::invalid_argument&) {
        // An inbox is structurally owned; no arbitrary parent ID routing.
    }
    if (inbox.pending() != 0 || inbox.dropped() != 1) return 27;
    try {
        (void)ActorFailureControlInbox("", 4);
        return 28;
    } catch (const std::invalid_argument&) {}
    try {
        (void)ActorFailureControlInbox("parent-1", 0);
        return 29;
    } catch (const std::invalid_argument&) {}
    try {
        (void)ActorFailureControlInbox("parent-1", 257);
        return 30;
    } catch (const std::invalid_argument&) {}

    // Fixed-size ring reuses slots after pop without duplicating or
    // reordering child notices, even after multiple wraparounds.
    ActorFailureControlInbox ring("parent-1", 2);
    const auto third = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-3", "parent-1");
    if (!ring.offer(raised) || !ring.offer(panicked)) return 31;
    if (ring.offer(third) || ring.dropped() != 1) return 32;
    if (ring.try_pop()->effect() != ActorExceptionalEffect::Raise) return 33;
    if (!ring.offer(third)) return 34;
    if (ring.try_pop()->effect() != ActorExceptionalEffect::Panic) return 35;
    if (ring.try_pop()->envelope().actor_id() != "actor-3") return 36;
    if (ring.try_pop() || ring.pending() != 0 || ring.dropped() != 1) return 37;
    for (int i = 0; i < 1024; ++i) {
        if (!ring.offer(raised)) return 38;
        auto item = ring.try_pop();
        if (!item || item->effect() != ActorExceptionalEffect::Raise) return 39;
    }
    if (ring.pending() != 0 || ring.dropped() != 1) return 40;
    return 0;
}
