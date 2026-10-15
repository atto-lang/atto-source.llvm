#include "oreslang/runtime/ActorErrorCatalog.hpp"
#include "oreslang/runtime/ActorFailureControl.hpp"
#include "oreslang/runtime/ExceptionPolicy.hpp"

#include <limits>
#include <stdexcept>
#include <string>

using namespace oreslang::runtime;

static_assert(saturated_control_count(
        std::numeric_limits<std::size_t>::max() - 1, 10)
        == std::numeric_limits<std::size_t>::max());
static_assert(saturated_control_count(0, 16) == 16);
static_assert(max_control_suppressed_nodes <= 16);
static_assert(max_control_suppressed_depth <= 8);

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

static std::size_t retained_suppressed(
        const Failure& failure, std::size_t depth = 0) {
    if (depth > max_control_suppressed_depth) {
        throw std::logic_error("native suppressed failure depth unbounded");
    }
    std::size_t count = 0;
    for (const auto& child : failure.suppressed()) {
        ++count;
        count += retained_suppressed(child, depth + 1);
    }
    return count;
}

int main() {
    // Failures are host metadata and must not accumulate unbounded suppression
    // under thousands of cleanup failures or repeated priority changes.
    auto bounded = Failure::make(FailureKind::Throw, "original guest failure");
    for (int i = 0; i < 4096; ++i) {
        bounded = combine_failure(
                std::move(bounded),
                Failure::make(FailureKind::Throw, "cleanup failure"));
        if (retained_suppressed(bounded) > max_control_suppressed_nodes) return 48;
    }
    if (bounded.kind() != FailureKind::Throw) return 49;
    if (bounded.suppressed_dropped() < 4096 - max_control_suppressed_nodes) return 50;

    bounded = combine_failure(
            std::move(bounded),
            Failure::make(FailureKind::Cancelled, "scheduler cancellation"));
    if (bounded.kind() != FailureKind::Cancelled) return 51;
    if (retained_suppressed(bounded) > max_control_suppressed_nodes) return 52;
    if (bounded.suppressed_dropped() < 4096 - max_control_suppressed_nodes) return 53;
    if (bounded.suppressed().size() != 1
            || bounded.suppressed().front().kind() != FailureKind::Throw) return 54;

    // Exhaustive finite transition oracle: four failures drawn from every
    // host priority class. Ties keep their first occurrence; later higher
    // priority failures must dominate without dropping boundedness invariants.
    constexpr FailureKind allKinds[] = {
        FailureKind::Throw, FailureKind::Raise, FailureKind::Panic,
        FailureKind::Cancelled, FailureKind::ProcessExit,
        FailureKind::HostFatal
    };
    for (auto k0 : allKinds) {
        for (auto k1 : allKinds) {
            for (auto k2 : allKinds) {
                for (auto k3 : allKinds) {
                    auto observed = Failure::make(k0, "first");
                    FailureKind winner = k0;
                    for (auto next : {k1, k2, k3}) {
                        if (failure_priority(next) > failure_priority(winner)) {
                            winner = next;
                        }
                        observed = combine_failure(
                                std::move(observed), Failure::make(next, "cleanup"));
                    }
                    if (observed.kind() != winner) return 55;
                    if (retained_suppressed(observed)
                            > max_control_suppressed_nodes) return 56;
                }
            }
        }
    }

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
    if (!inbox.register_direct_child("actor-1")) return 63;
    if (!inbox.offer(raised, "actor-1")) return 19;
    if (inbox.pending() != 1) return 20;
    if (inbox.offer(panicked, "actor-1")) return 21;
    if (inbox.dropped() != 1) return 22;
    const auto received = inbox.try_pop();
    if (!received || received->effect() != ActorExceptionalEffect::Raise) return 23;
    if (received->envelope().actor_id() != "actor-1") return 24;
    if (inbox.try_pop()) return 25;

    const auto foreign_child = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-2", "parent-2");
    try {
        (void)inbox.offer(foreign_child, "actor-2");
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
    if (!ring.register_direct_child("actor-1")
            || !ring.register_direct_child("actor-3")) return 64;
    const auto third = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-3", "parent-1");
    if (!ring.offer(raised, "actor-1") || !ring.offer(panicked, "actor-1")) return 31;
    if (ring.offer(third, "actor-3") || ring.dropped() != 1) return 32;
    if (ring.try_pop()->effect() != ActorExceptionalEffect::Raise) return 33;
    if (!ring.offer(third, "actor-3")) return 34;
    if (ring.try_pop()->effect() != ActorExceptionalEffect::Panic) return 35;
    if (ring.try_pop()->envelope().actor_id() != "actor-3") return 36;
    if (ring.try_pop() || ring.pending() != 0 || ring.dropped() != 1) return 37;
    for (int i = 0; i < 1024; ++i) {
        if (!ring.offer(raised, "actor-1")) return 38;
        auto item = ring.try_pop();
        if (!item || item->effect() != ActorExceptionalEffect::Raise) return 39;
    }
    if (ring.pending() != 0 || ring.dropped() != 1) return 40;

    // A retired supervisor cannot receive another child CONTROL notice;
    // unread notices are counted as dropped and cannot be resurrected.
    ActorFailureControlInbox retiring("parent-1", 2);
    if (!retiring.register_direct_child("actor-1")
            || !retiring.register_direct_child("actor-3")) return 65;
    if (!retiring.offer(raised, "actor-1") || !retiring.offer(panicked, "actor-1")) return 41;
    if (retiring.pending() != 2 || retiring.dropped() != 0) return 42;
    retiring.retire();
    if (!retiring.retired() || retiring.pending() != 0) return 43;
    if (retiring.dropped() != 2 || retiring.try_pop()) return 44;
    retiring.retire();
    if (retiring.dropped() != 2) return 45;
    if (retiring.offer(third, "actor-3")) return 46;
    if (retiring.pending() != 0 || retiring.dropped() != 3) return 47;

    // An envelope's claimed parent alone is insufficient. The runtime must
    // independently prove which direct child is being reported.
    ActorFailureControlInbox authorized("parent-1", 2);
    try {
        (void)authorized.offer(raised, "actor-2");
        return 57;
    } catch (const std::invalid_argument&) {}
    try {
        (void)authorized.offer(raised, "");
        return 58;
    } catch (const std::invalid_argument&) {}
    try {
        (void)authorized.offer(raised, "parent-1");
        return 59;
    } catch (const std::invalid_argument&) {}
    if (authorized.pending() || authorized.dropped()) return 60;
    try {
        (void)authorized.offer(raised, "actor-1");
        return 66;
    } catch (const std::invalid_argument&) {
        // A matching pair of IDs is not sufficient without registration.
    }
    if (!authorized.register_direct_child("actor-1")) return 67;
    if (authorized.register_direct_child("actor-1")) return 68;
    if (!authorized.offer(raised, "actor-1")) return 61;
    if (!authorized.unregister_direct_child("actor-1")) return 69;
    if (authorized.unregister_direct_child("actor-1")) return 70;
    const auto admitted = authorized.try_pop();
    if (!admitted || admitted->envelope().actor_id() != "actor-1") return 71;
    try {
        (void)authorized.offer(raised, "actor-1");
        return 72;
    } catch (const std::invalid_argument&) {}
    try {
        (void)authorized.register_direct_child("parent-1");
        return 73;
    } catch (const std::invalid_argument&) {}
    try {
        (void)authorized.register_direct_child("");
        return 74;
    } catch (const std::invalid_argument&) {}
    authorized.retire();
    if (authorized.register_direct_child("actor-1")) return 75;
    if (authorized.pending() != 0 || !authorized.retired()) return 76;

    try {
        (void)ActorFailureControlNotice::from_guest_effect(
                ActorExceptionalEffect::Raise, "same-id", "same-id");
        return 62;
    } catch (const std::invalid_argument&) {}
    return 0;
}
