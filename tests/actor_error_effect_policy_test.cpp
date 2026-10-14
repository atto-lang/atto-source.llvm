#include "oreslang/runtime/ActorErrorCatalog.hpp"
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
    return 0;
}
