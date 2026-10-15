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
    const auto inboxPermit = inbox.register_direct_child("actor-1");
    if (!inboxPermit) return 63;
    if (!inbox.offer(raised, *inboxPermit)) return 19;
    if (inbox.pending() != 1) return 20;
    if (inbox.offer(panicked, *inboxPermit)) return 21;
    // The first accepted terminal effect fences all later attempts from the
    // same child, even if its parent has not consumed the original notice.
    if (inbox.duplicate_rejections() != 1 || inbox.dropped() != 0) return 22;
    const auto inboxThree = inbox.register_direct_child("actor-3");
    if (!inboxThree) return 82;
    if (inbox.offer(ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-3", "parent-1"), *inboxThree)) return 83;
    if (inbox.dropped() != 1) return 84;
    const auto received = inbox.try_pop();
    if (!received || received->effect() != ActorExceptionalEffect::Raise) return 23;
    if (received->envelope().actor_id() != "actor-1") return 24;
    if (inbox.try_pop()) return 25;

    const auto foreign_child = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-2", "parent-2");
    try {
        (void)inbox.offer(foreign_child, *inboxPermit);
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
    const auto ringOne = ring.register_direct_child("actor-1");
    const auto ringThree = ring.register_direct_child("actor-3");
    const auto ringFour = ring.register_direct_child("actor-4");
    if (!ringOne || !ringThree || !ringFour) return 64;
    const auto third = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "actor-3", "parent-1");
    const auto fourth = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Panic, "actor-4", "parent-1");
    if (!ring.offer(raised, *ringOne) || !ring.offer(fourth, *ringFour)) return 31;
    if (ring.offer(third, *ringThree) || ring.dropped() != 1) return 32;
    if (ring.try_pop()->effect() != ActorExceptionalEffect::Raise) return 33;
    if (!ring.offer(third, *ringThree)) return 34;
    if (ring.try_pop()->effect() != ActorExceptionalEffect::Panic) return 35;
    if (ring.try_pop()->envelope().actor_id() != "actor-3") return 36;
    if (ring.try_pop() || ring.pending() != 0 || ring.dropped() != 1) return 37;
    // Recycle ring slots with fresh actor identities (not duplicate terminal
    // notifications from the same child generation).
    for (int i = 0; i < 1024; ++i) {
        const std::string child = "cycle-" + std::to_string(i);
        const auto next = ActorFailureControlNotice::from_guest_effect(
                ActorExceptionalEffect::Raise, child, "parent-1");
        const auto cyclePermit = ring.register_direct_child(child);
        if (!cyclePermit || !ring.offer(next, *cyclePermit)) return 38;
        auto item = ring.try_pop();
        if (!item || item->effect() != ActorExceptionalEffect::Raise
                || item->envelope().actor_id() != child) return 39;
        if (!ring.unregister_direct_child(*cyclePermit)) return 85;
    }
    if (ring.pending() != 0 || ring.dropped() != 1
            || ring.duplicate_rejections() != 0) return 40;

    // A retired supervisor cannot receive another child CONTROL notice;
    // unread notices are counted as dropped and cannot be resurrected.
    ActorFailureControlInbox retiring("parent-1", 2);
    const auto retiringOne = retiring.register_direct_child("actor-1");
    const auto retiringThree = retiring.register_direct_child("actor-3");
    if (!retiringOne || !retiringThree) return 65;
    if (!retiring.offer(raised, *retiringOne) || !retiring.offer(third, *retiringThree)) return 41;
    if (retiring.pending() != 2 || retiring.dropped() != 0) return 42;
    retiring.retire();
    if (!retiring.retired() || retiring.pending() != 0) return 43;
    if (retiring.dropped() != 2 || retiring.try_pop()) return 44;
    retiring.retire();
    if (retiring.dropped() != 2) return 45;
    if (retiring.offer(third, *retiringThree)) return 46;
    if (retiring.pending() != 0 || retiring.dropped() != 3) return 47;

    // An envelope's claimed parent alone is insufficient. The runtime must
    // independently prove which direct child is being reported.
    ActorFailureControlInbox authorized("parent-1", 2);
    // A valid permit from a different inbox, even with the same supervisor
    // string, cannot grant rights in this incarnation.
    try {
        (void)authorized.offer(raised, *inboxPermit);
        return 57;
    } catch (const std::invalid_argument&) {}
    if (authorized.unregister_direct_child(*inboxPermit)) return 58;
    const auto wrongActor = authorized.register_direct_child("actor-2");
    if (!wrongActor) return 59;
    try {
        (void)authorized.offer(raised, *wrongActor);
        return 66;
    } catch (const std::invalid_argument&) {}
    if (!authorized.unregister_direct_child(*wrongActor)) return 91;
    if (authorized.pending() || authorized.dropped()) return 60;
    const auto current = authorized.register_direct_child("actor-1");
    if (!current) return 67;
    if (authorized.register_direct_child("actor-1")) return 68;
    if (!authorized.offer(raised, *current)) return 61;
    if (authorized.offer(panicked, *current)) return 86;
    if (authorized.duplicate_rejections() != 1 || authorized.dropped() != 0) return 87;
    if (authorized.try_pop()->effect() != ActorExceptionalEffect::Raise) return 88;
    if (authorized.offer(raised, *current)) return 89; // replay after drain
    if (authorized.duplicate_rejections() != 2) return 90;
    if (!authorized.unregister_direct_child(*current)) return 69;
    if (authorized.unregister_direct_child(*current)) return 70;
    if (authorized.try_pop()) return 71;
    // A stale permit must not revive or revoke a new incarnation with reused ID.
    const auto replacement = authorized.register_direct_child("actor-1");
    if (!replacement) return 92;
    try {
        (void)authorized.offer(raised, *current);
        return 72;
    } catch (const std::invalid_argument&) {}
    if (authorized.unregister_direct_child(*current)) return 93;
    if (!authorized.offer(raised, *replacement)) return 94;
    if (!authorized.try_pop()) return 95;
    if (!authorized.unregister_direct_child(*replacement)) return 96;
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

    // An old child's accepted notice can survive unregister while a new
    // incarnation reuses the same opaque ActorId. Dequeued CONTROL data must
    // identify which generation actually emitted the terminal event.
    ActorFailureControlInbox reusedId("parent-1", 2);
    const auto oldPermit = reusedId.register_direct_child("reused");
    if (!oldPermit) return 97;
    const auto oldNotice = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "reused", "parent-1");
    if (oldNotice.admitted_inbox_incarnation() != 0
            || oldNotice.admitted_child_generation() != 0) return 98;
    if (!reusedId.offer(oldNotice, *oldPermit)) return 99;
    if (!reusedId.unregister_direct_child(*oldPermit)) return 100;
    const auto newPermit = reusedId.register_direct_child("reused");
    if (!newPermit) return 101;
    if (!reusedId.offer(ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Panic, "reused", "parent-1"), *newPermit)) return 102;
    const auto oldDelivered = reusedId.try_pop();
    const auto newDelivered = reusedId.try_pop();
    if (!oldDelivered || !newDelivered || reusedId.try_pop()) return 103;
    if (oldDelivered->admitted_inbox_incarnation() == 0
            || oldDelivered->admitted_inbox_incarnation()
                    != newDelivered->admitted_inbox_incarnation()
            || oldDelivered->admitted_child_generation() == 0
            || oldDelivered->admitted_child_generation()
                    == newDelivered->admitted_child_generation()) return 104;
    if (reusedId.belongs_to_current_child(*oldDelivered, *newPermit)
            || reusedId.belongs_to_current_child(*oldDelivered, *oldPermit)
            || !reusedId.belongs_to_current_child(*newDelivered, *newPermit)
            || reusedId.belongs_to_current_child(*newDelivered, *oldPermit)) return 105;
    try {
        (void)reusedId.offer(*oldDelivered, *newPermit);
        return 106;
    } catch (const std::invalid_argument&) {}
    if (!reusedId.unregister_direct_child(*newPermit)
            || reusedId.belongs_to_current_child(*newDelivered, *newPermit)) return 107;
    if (authorized.belongs_to_current_child(*newDelivered, *current)) return 108;
    if (reusedId.pending() || reusedId.dropped()) return 109;

    // Spawn-time registry growth is finite even when the CONTROL ring is tiny.
    // A retired or full supervisor cannot accidentally accept new lineage.
    ActorFailureControlInbox boundedChildren("bounded-parent", 1);
    std::optional<ActorFailureControlInbox::DirectChildPermit> firstPermit;
    for (int i = 0; i < 4096; ++i) {
        auto permit = boundedChildren.register_direct_child("child-" + std::to_string(i));
        if (!permit) return 77;
        if (i == 0) firstPermit = permit;
    }
    if (boundedChildren.register_direct_child("overflow-child")) return 78;
    if (!firstPermit || !boundedChildren.unregister_direct_child(*firstPermit)) return 79;
    if (!boundedChildren.register_direct_child("replacement-child")) return 80;
    boundedChildren.retire();
    if (boundedChildren.register_direct_child("post-retirement")) return 81;

    try {
        (void)ActorFailureControlNotice::from_guest_effect(
                ActorExceptionalEffect::Raise, "same-id", "same-id");
        return 62;
    } catch (const std::invalid_argument&) {}
    return 0;
}
