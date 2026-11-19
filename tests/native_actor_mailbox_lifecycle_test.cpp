#include "oreslang/runtime/PriorityActorInbox.hpp"

#include <iostream>
#include <stdexcept>
#include <optional>
#include <utility>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::PriorityActorInbox;

#define CHECK(x) do { if (!(x)) throw std::runtime_error("postmerge mailbox lifecycle check failed: " #x); } while (false)

int main() {
    using Inbox = PriorityActorInbox<int>;
    using Lane = Inbox::Lane;

    NativeActorGroup group(9901, 1);
    auto actor = group.adopt(9901, 77, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    CHECK(group.mark_ready(actor));

    Inbox inbox({2, 16}, {2, 16}, {2, 16}, 4);
    inbox.bind_target(actor);

    // Sequence 1 is NORMAL; sequence 2 is URGENT. Priority dequeue may
    // overtake publication order, but completion must never overtake the
    // contiguous global drain fence.
    CHECK(inbox.offer_to(actor, Lane::Normal, 11, 8));
    CHECK(inbox.offer_to(actor, Lane::Urgent, 22, 8));
    CHECK(inbox.snapshot() == 2);
    CHECK(actor.outstanding_resource_loans() == 2);

    // A bound inbox may only dequeue under a real generation-fenced
    // NativeActorGroup user turn; raw receive is forbidden.
    bool raw_denied = false;
    try { (void)inbox.try_receive(); }
    catch (const std::logic_error&) { raw_denied = true; }
    CHECK(raw_denied);
    std::optional<Inbox::Delivery> checked_out;
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery& receipt) {
        checked_out.emplace(std::move(receipt));
    }) == Inbox::Dispatch::User);
    CHECK(checked_out);
    CHECK(checked_out->lane() == Lane::Urgent);
    CHECK(checked_out->sequence() == 2);
    CHECK(checked_out->value() == 22);

    // STOP/detach establishes the turn fence, but physical DONE must remain
    // blocked by both the queued receipt and the checked-out receipt.
    group.close();
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(!finalizer.ready_for_resource_reclamation());
    CHECK(!finalizer.finalized("mailbox receipts still own resources"));

    // Terminal cleanup may abort only queued work. The checked-out owner
    // remains authoritative until it explicitly acknowledges quiescence.
    CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 1);
    CHECK(inbox.watermark() == 1);
    CHECK(inbox.drain_status(1) == Inbox::DrainStatus::Aborted);
    CHECK(inbox.drain_status(2) == Inbox::DrainStatus::Pending);
    CHECK(actor.outstanding_resource_loans() == 1);
    CHECK(!finalizer.ready_for_resource_reclamation());
    CHECK(!finalizer.finalized("checked-out receipt still owns resources"));

    CHECK(checked_out->acknowledge_released());
    CHECK(!checked_out->acknowledge_released());
    CHECK(inbox.watermark() == 2);
    // A terminally aborted earlier sequence poisons a drain through later
    // successful ACKs; teardown must never be misreported as delivery.
    CHECK(inbox.drain_status(2) == Inbox::DrainStatus::Aborted);
    CHECK(!inbox.completed_through(2));
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(finalizer.ready_for_resource_reclamation());

    CHECK(finalizer.finalized());
    CHECK(!finalizer.finalized());
    actor.get_done().get();

    // Once terminal cleanup seals the inbox, the old actor capability cannot
    // resurrect admission or move the producer payload.
    int retained = 33;
    bool rejected = false;
    try {
        (void)inbox.offer_to(actor, Lane::Bulk, std::move(retained), 8);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    CHECK(rejected);
    CHECK(retained == 33);
    CHECK(actor.outstanding_resource_loans() == 0);
    inbox.close();

    std::cout << "POSTMERGE_MAILBOX_LIFECYCLE_PASS\n";
}
