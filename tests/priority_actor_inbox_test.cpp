#include "oreslang/runtime/PriorityActorInbox.hpp"
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/SupervisorControlTurn.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include <string>
#include <type_traits>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <future>

using oreslang::runtime::PriorityActorInbox;
using oreslang::runtime::NativeActorGroup;
using oreslang::SupervisorControlTurn;
using oreslang::SupervisorFailureLane;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("priority inbox check: " #condition); } while (false)

using Inbox = PriorityActorInbox<int>;
using Lane = Inbox::Lane;
static_assert(!std::is_move_assignable_v<Inbox::Delivery>,
              "a checked-out priority delivery may not be overwritten");

struct SupervisorHooks final : SupervisorControlTurn::Hooks {
    int terminal = 0;
    int user = 0;
    SupervisorControlTurn::Decision on_throw(const SupervisorFailureLane::Failure&) override {
        throw std::logic_error("terminal Throw must be promoted");
    }
    SupervisorControlTurn::Decision on_raise(const SupervisorFailureLane::Failure&) override {
        throw std::logic_error("terminal Raise must be promoted");
    }
    SupervisorControlTurn::Decision on_panic(const SupervisorFailureLane::Failure&) override {
        ++terminal; return SupervisorControlTurn::Decision::Acknowledge;
    }
    SupervisorControlTurn::Decision on_cancel(const SupervisorFailureLane::Failure&) override {
        ++terminal; return SupervisorControlTurn::Decision::Acknowledge;
    }
    SupervisorControlTurn::Decision on_host_failure(const SupervisorFailureLane::Failure&) override {
        ++terminal; return SupervisorControlTurn::Decision::Acknowledge;
    }
    void on_escalate(const SupervisorFailureLane::Failure&) override {
        throw std::logic_error("unexpected escalation");
    }
    bool on_restart(const SupervisorFailureLane::Failure&) override { return false; }
    void on_user_turn() override { ++user; }
};

static void bounded_lanes_and_deterministic_fairness() {
    Inbox inbox({16, 128}, {8, 64}, {2, 16});
    for (int n = 0; n < 16; ++n) CHECK(inbox.offer(Lane::Urgent, int(n), 8));
    for (int n = 0; n < 8; ++n) CHECK(inbox.offer(Lane::Normal, 100 + n, 8));
    for (int n = 0; n < 2; ++n) CHECK(inbox.offer(Lane::Bulk, 200 + n, 8));
    CHECK(!inbox.offer(Lane::Urgent, 999, 8));
    CHECK(!inbox.offer(Lane::Normal, 999, 8));
    CHECK(!inbox.offer(Lane::Bulk, 999, 8));
    CHECK(inbox.used_bytes(Lane::Urgent) == 128);
    CHECK(inbox.used_bytes(Lane::Normal) == 64);
    CHECK(inbox.used_bytes(Lane::Bulk) == 16);
    int seen_urgent = 0, seen_normal = 0, seen_bulk = 0;
    for (int turn = 0; turn < 26; ++turn) {
        auto item = inbox.try_receive();
        CHECK(item.has_value());
        const int weight_slot = turn % 13;
        const Lane expected = weight_slot < 8 ? Lane::Urgent
                : weight_slot < 12 ? Lane::Normal : Lane::Bulk;
        CHECK(item->lane() == expected);
        if (expected == Lane::Urgent) CHECK(item->value() == seen_urgent++);
        if (expected == Lane::Normal) CHECK(item->value() == 100 + seen_normal++);
        if (expected == Lane::Bulk) CHECK(item->value() == 200 + seen_bulk++);
        CHECK(inbox.used_frames(expected) > 0); // dequeue never refunds
        CHECK(item->charged_bytes() == 8);
        CHECK(item->acknowledge_released());
        CHECK(!item->acknowledge_released());
    }
    CHECK(seen_urgent == 16 && seen_normal == 8 && seen_bulk == 2);
    CHECK(!inbox.try_receive());
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    inbox.close();
}

static void sustained_urgent_flood_cannot_starve_normal_or_bulk() {
    Inbox inbox({16, 128}, {8, 64}, {2, 16});
    for (int i = 0; i < 16; ++i) CHECK(inbox.offer(Lane::Urgent, int(i), 8));
    CHECK(inbox.offer(Lane::Normal, 1000, 8));
    CHECK(inbox.offer(Lane::Bulk, 2000, 8));
    int normal_at = -1, bulk_at = -1;
    for (int tick = 1; tick <= 52; ++tick) {
        auto message = inbox.try_receive();
        CHECK(message.has_value());
        Lane lane = message->lane();
        CHECK(message->acknowledge_released());
        if (lane == Lane::Urgent) CHECK(inbox.offer(Lane::Urgent, 42, 8));
        if (lane == Lane::Normal && normal_at == -1) normal_at = tick;
        if (lane == Lane::Bulk && bulk_at == -1) bulk_at = tick;
    }
    CHECK(normal_at > 0 && normal_at <= 9);
    CHECK(bulk_at > 0 && bulk_at <= 13);
    // Drain remaining admitted URGENT messages and prove exact refund.
    while (auto item = inbox.try_receive()) CHECK(item->acknowledge_released());
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    inbox.close();
}

static void seal_rejects_all_lanes_but_keeps_admitted_receipts_drainable() {
    Inbox inbox({1, 8}, {1, 8}, {1, 8});
    CHECK(inbox.offer(Lane::Urgent, 1, 8));
    CHECK(inbox.offer(Lane::Normal, 2, 8));
    CHECK(inbox.offer(Lane::Bulk, 3, 8));
    bool outstanding = false;
    try { inbox.close(); }
    catch (const std::logic_error&) { outstanding = true; }
    CHECK(outstanding);
    for (const auto lane : {Lane::Urgent, Lane::Normal, Lane::Bulk}) {
        bool rejected = false;
        try { (void)inbox.offer(lane, 55, 8); }
        catch (const std::logic_error&) { rejected = true; }
        CHECK(rejected);
    }
    int count = 0;
    while (auto receipt = inbox.try_receive()) {
        CHECK(receipt->acknowledge_released());
        ++count;
    }
    CHECK(count == 3);
    inbox.close();

    Inbox bad({1, 8}, {1, 8}, {1, 8});
    bool invalid = false;
    try { (void)bad.offer(static_cast<Lane>(255), 1, 8); }
    catch (const std::invalid_argument&) { invalid = true; }
    CHECK(invalid);
    bad.close();
}

static void supervisor_control_and_actor_single_flight_precede_user_priority() {
    Inbox inbox({1, 8}, {1, 8}, {1, 8});
    CHECK(inbox.offer(Lane::Urgent, 1, 8));
    CHECK(inbox.offer(Lane::Normal, 2, 8));
    CHECK(inbox.offer(Lane::Bulk, 3, 8));
    SupervisorControlTurn supervisor(1);
    SupervisorHooks hooks;
    SupervisorFailureLane::ActorKey child{"child", 4};
    supervisor.reserve_child(child);
    CHECK(supervisor.publish_child(child, SupervisorFailureLane::Kind::Panic, "broken"));
    CHECK(supervisor.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.terminal == 1 && hooks.user == 0);
    CHECK(inbox.used_frames(Lane::Urgent) == 1);
    CHECK(inbox.used_frames(Lane::Normal) == 1);
    CHECK(inbox.used_frames(Lane::Bulk) == 1);

    NativeActorGroup group(601, 1);
    auto actor = group.adopt(601, 12, 2, [] {});
    CHECK(group.mark_ready(actor));
    int control = 0, user = 0;
    CHECK(group.enqueue_control(actor, [&] { ++control; }));
    CHECK(group.run_user_turn(actor, [&] { ++user; }) ==
          NativeActorGroup::UserTurn::busy);
    CHECK(group.run_control_turn(actor) == NativeActorGroup::ControlTurn::executed);
    CHECK(control == 1 && user == 0);
    CHECK(group.run_user_turn(actor, [&] {
        auto message = inbox.try_receive();
        CHECK(message && message->lane() == Lane::Urgent);
        CHECK(message->acknowledge_released());
        ++user;
    }) == NativeActorGroup::UserTurn::executed);
    CHECK(user == 1);
    for (int i = 0; i < 2; ++i) {
        auto message = inbox.try_receive();
        CHECK(message && message->acknowledge_released());
    }
    CHECK(group.finalized(actor));
    actor.get_done().get();
    inbox.close();
}


static void actual_native_dispatch_obeys_control_and_logical_actor_turn() {
    NativeActorGroup group(700, 1);
    auto actor = group.adopt(700, 5, 4, [] {});
    Inbox inbox({2, 16}, {2, 16}, {2, 16});
    inbox.bind_target(actor);
    CHECK(inbox.offer_to(actor, Lane::Normal, 1, 8));
    CHECK(inbox.offer_to(actor, Lane::Urgent, 2, 8));
    CHECK(inbox.offer_to(actor, Lane::Bulk, 3, 8));
    std::vector<int> delivered;
    auto consume = [&](Inbox::Delivery& receipt) {
        delivered.push_back(receipt.value());
        CHECK(receipt.acknowledge_released());
    };
    // Unready actor may not run guest mail, even if messages were buffered.
    CHECK(inbox.dispatch_next(group, actor, consume) == Inbox::Dispatch::Retired);
    CHECK(group.mark_ready(actor));
    int controls = 0;
    CHECK(group.enqueue_control(actor, [&] { ++controls; }));
    CHECK(inbox.dispatch_next(group, actor, consume) == Inbox::Dispatch::Control);
    CHECK(controls == 1 && delivered.empty());
    CHECK(inbox.dispatch_next(group, actor, consume) == Inbox::Dispatch::User);
    CHECK(delivered == std::vector<int>({2}));
    CHECK(inbox.watermark() == 0); // earlier NORMAL remains unresolved
    CHECK(inbox.dispatch_next(group, actor, consume) == Inbox::Dispatch::User);
    CHECK(delivered == std::vector<int>({2, 1}));
    CHECK(inbox.watermark() == 2);
    CHECK(inbox.dispatch_next(group, actor, consume) == Inbox::Dispatch::User);
    CHECK(delivered == std::vector<int>({2, 1, 3}));
    CHECK(inbox.dispatch_next(group, actor, consume) == Inbox::Dispatch::Idle);
    CHECK(inbox.completed_through(3));
    CHECK(group.finalized(actor));
    actor.get_done().get();
    inbox.close();
}

static void native_dispatch_does_not_steal_mail_from_active_carrier() {
    NativeActorGroup group(701, 1);
    auto actor = group.adopt(701, 5, 4, [] {});
    CHECK(group.mark_ready(actor));
    Inbox inbox({1, 8}, {1, 8}, {1, 8});
    inbox.bind_target(actor);
    CHECK(inbox.offer_to(actor, Lane::Urgent, 77, 8));
    std::promise<void> active, release;
    auto ready = active.get_future();
    auto gate = release.get_future().share();
    std::thread carrier([&] {
        CHECK(group.run_user_turn(actor, [&] {
            active.set_value();
            gate.wait(); // host-only deterministic race; not guest scheduling
        }) == NativeActorGroup::UserTurn::executed);
    });
    ready.wait();
    int received = 0;
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery&) { ++received; })
          == Inbox::Dispatch::Busy);
    CHECK(received == 0 && inbox.used_frames(Lane::Urgent) == 1);
    release.set_value();
    carrier.join();
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery& receipt) {
        CHECK(receipt.value() == 77);
        CHECK(receipt.acknowledge_released());
        ++received;
    }) == Inbox::Dispatch::User);
    CHECK(received == 1 && inbox.watermark() == 1);
    CHECK(group.finalized(actor));
    actor.get_done().get();
    inbox.close();
}

// This uses the real NativeActorGroup implementation (not an API stub).
// The host-only promise gates establish ordering; production actor scheduling
// must never block a carrier on a future or condition_variable.
static void closed_group_never_reclaims_saturated_inbox_with_active_user_turn() {
    NativeActorGroup group(8841, 1);
    std::atomic<int> stop_requests{0};
    Inbox inbox({1, 8}, {1, 8}, {1, 8}, 3);
    auto actor = group.adopt(8841, 19, 7, [&] {
        ++stop_requests;
        // Trusted STOP hook seals admission immediately, then the runtime
        // waits for physical actor-turn quiescence before retiring payloads.
        try { inbox.close(); }
        catch (const std::logic_error&) { /* owned frames remain outstanding */ }
    });
    auto finalizer = group.runtime_finalizer(actor);
    CHECK(group.mark_ready(actor));
    inbox.bind_target(actor);
    CHECK(inbox.offer_to(actor, Lane::Normal, 101, 8)); // sequence 1
    CHECK(inbox.offer_to(actor, Lane::Urgent, 202, 8)); // sequence 2
    CHECK(inbox.offer_to(actor, Lane::Bulk, 303, 8));   // sequence 3
    CHECK(inbox.snapshot() == 3);
    CHECK(!inbox.offer_to(actor, Lane::Normal, 404, 8));
    CHECK(!inbox.offer_to(actor, Lane::Urgent, 404, 8));
    CHECK(!inbox.offer_to(actor, Lane::Bulk, 404, 8));

    std::promise<void> entered, allowed_to_finish;
    auto entered_future = entered.get_future();
    auto exit_gate = allowed_to_finish.get_future().share();
    std::atomic<int> user_turns{0};
    std::thread carrier([&] {
        CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery& receipt) {
            CHECK(receipt.lane() == Lane::Urgent);
            CHECK(receipt.sequence() == 2 && receipt.value() == 202);
            ++user_turns;
            entered.set_value();
            exit_gate.wait(); // TEST ONLY: emulate an active host carrier
            CHECK(receipt.acknowledge_released());
        }) == Inbox::Dispatch::User);
    });
    entered_future.wait();
    CHECK(user_turns.load() == 1);
    CHECK(group.enqueue_control(actor, [] {}));
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery&) {
        CHECK(false && "CONTROL-pending actor must not admit another USER");
    }) == Inbox::Dispatch::Busy);
    CHECK(inbox.watermark() == 0);
    CHECK(!finalizer.ready_for_resource_reclamation());
    CHECK(!group.finalized(actor)); // CONTROL reserved and user active

    // close() never waits on an actor carrier: STOP callback is host-only.
    group.close();
    CHECK(group.closed());
    CHECK(stop_requests.load() == 1);
    int refused_payload = 909;
    bool stop_refused_send = false;
    try { (void)inbox.offer_to(actor, Lane::Normal, std::move(refused_payload), 8); }
    catch (const std::logic_error&) { stop_refused_send = true; }
    CHECK(stop_refused_send && refused_payload == 909);
    CHECK(actor.control_delivery_lost()); // pending CONTROL dropped, not ACKed
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery&) {
        CHECK(false && "closed actor received user mail");
    }) == Inbox::Dispatch::Retired);
    CHECK(!finalizer.ready_for_resource_reclamation());
    CHECK(!finalizer.finalized("actor turn still active"));
    bool denied = false;
    try { (void)inbox.retire_queued_after_quiescence(); }
    catch (const std::logic_error&) { denied = true; }
    CHECK(denied); // a bound inbox never accepts the unguarded retirement API
    denied = false;
    try { (void)inbox.retire_queued_after_quiescence(finalizer, actor); }
    catch (const std::logic_error&) { denied = true; }
    CHECK(denied); // STOP is not proof that the user turn has physically exited
    CHECK(inbox.used_frames(Lane::Urgent) == 1);
    CHECK(inbox.used_frames(Lane::Normal) == 1);
    CHECK(inbox.used_frames(Lane::Bulk) == 1);

    allowed_to_finish.set_value();
    carrier.join();
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(!finalizer.ready_for_resource_reclamation());
    CHECK(actor.outstanding_resource_loans() == 2);
    CHECK(!finalizer.finalized("queued receipts still own memory"));
    CHECK(inbox.watermark() == 0); // earlier NORMAL has not finished
    CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 2);
    CHECK(inbox.watermark() == 3);
    CHECK(inbox.drain_status(1) == Inbox::DrainStatus::Aborted);
    CHECK(inbox.drain_status(2) == Inbox::DrainStatus::Aborted);
    CHECK(inbox.drain_status(3) == Inbox::DrainStatus::Aborted);
    CHECK(!inbox.completed_through(3));
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 0);
    inbox.close();

    CHECK(finalizer.finalized());
    CHECK(!finalizer.finalized());
    actor.get_done().get();
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery&) {
        CHECK(false && "finalized actor received user mail");
    }) == Inbox::Dispatch::Retired);
    CHECK(user_turns.load() == 1);
}

static void physically_checked_out_receipt_blocks_finalization_after_stop() {
    NativeActorGroup group(8893, 1);
    auto actor = group.adopt(8893, 21, 1, [] {});
    auto finalizer = group.runtime_finalizer(actor);
    CHECK(group.mark_ready(actor));
    Inbox inbox({2, 16}, {2, 16}, {2, 16}, 3);
    inbox.bind_target(actor);
    CHECK(inbox.offer_to(actor, Lane::Normal, 81, 8));
    CHECK(actor.outstanding_resource_loans() == 1);
    bool raw_denied = false;
    try { (void)inbox.try_receive(); }
    catch (const std::logic_error&) { raw_denied = true; }
    CHECK(raw_denied);
    std::optional<Inbox::Delivery> checked_out;
    CHECK(inbox.dispatch_next(group, actor, [&](Inbox::Delivery& receipt) {
        checked_out.emplace(std::move(receipt));
    }) == Inbox::Dispatch::User);
    CHECK(checked_out && checked_out->sequence() == 1);
    CHECK(inbox.used_frames(Lane::Normal) == 1);
    CHECK(!group.finalized(actor)); // handler exited, receipt has not
    group.close();
    CHECK(finalizer.ready_for_actor_turn_quiescence());
    CHECK(!finalizer.ready_for_resource_reclamation());
    CHECK(!finalizer.finalized("unreleased checked-out receipt"));
    CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) == 0);
    CHECK(actor.outstanding_resource_loans() == 1);
    CHECK(checked_out->acknowledge_released());
    CHECK(!checked_out->acknowledge_released());
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(inbox.completed_through(1));
    CHECK(finalizer.ready_for_resource_reclamation());
    CHECK(finalizer.finalized());
    CHECK(!finalizer.finalized());
    actor.get_done().get();
    bool rejected = false;
    try { actor.retain_resource_loan(); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    inbox.close();
}

static void terminal_stop_fails_drain_instead_of_reporting_dropped_mail_success() {
    Inbox inbox({2, 16}, {2, 16}, {2, 16});
    CHECK(inbox.offer(Lane::Normal, 101, 8));  // global sequence 1
    CHECK(inbox.offer(Lane::Urgent, 202, 8));  // global sequence 2
    CHECK(inbox.offer(Lane::Bulk, 303, 8));    // global sequence 3
    CHECK(inbox.snapshot() == 3);
    auto urgent = inbox.try_receive();
    CHECK(urgent && urgent->lane() == Lane::Urgent && urgent->sequence() == 2);
    CHECK(urgent->acknowledge_released());
    CHECK(inbox.watermark() == 0);
    CHECK(inbox.retire_queued_after_quiescence() == 2);
    CHECK(inbox.watermark() == 3);
    CHECK(inbox.drain_status(0) == Inbox::DrainStatus::Delivered);
    CHECK(inbox.drain_status(1) == Inbox::DrainStatus::Aborted);
    CHECK(inbox.drain_status(2) == Inbox::DrainStatus::Aborted);
    CHECK(inbox.drain_status(3) == Inbox::DrainStatus::Aborted);
    CHECK(!inbox.completed_through(3));
    CHECK(!inbox.try_receive());
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    CHECK(inbox.retire_queued_after_quiescence() == 0);
    bool sealed = false;
    try { (void)inbox.offer(Lane::Bulk, 404, 8); }
    catch (const std::logic_error&) { sealed = true; }
    CHECK(sealed);
    inbox.close();
}

static void terminal_retirement_waits_for_inflight_owner_to_ack() {
    Inbox inbox({2, 16}, {2, 16}, {2, 16});
    CHECK(inbox.offer(Lane::Normal, 11, 8));   // first
    auto inflight = inbox.try_receive();
    CHECK(inflight && inflight->sequence() == 1);
    CHECK(inbox.offer(Lane::Urgent, 22, 8));  // second
    const auto drain = inbox.snapshot();
    CHECK(inbox.retire_queued_after_quiescence() == 1);
    CHECK(inbox.watermark() == 0);
    CHECK(inbox.drain_status(drain) == Inbox::DrainStatus::Pending);
    CHECK(inbox.used_frames(Lane::Normal) == 1);
    CHECK(inflight->acknowledge_released());
    CHECK(inbox.watermark() == drain);
    CHECK(inbox.drain_status(drain) == Inbox::DrainStatus::Aborted);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(!inflight->retire_aborted_after_quiescence()); // no double release
    inbox.close();
}

static void stale_generation_cannot_reuse_a_live_native_actor_inbox() {
    NativeActorGroup old_group(750, 1), newer_group(750, 1);
    auto old_actor = old_group.adopt(750, 9, 1, [] {});
    // A newer generation cannot overlap a live incarnation's exclusive owner.
    bool duplicate_owner_rejected = false;
    try { (void)newer_group.adopt(750, 9, 2, [] {}); }
    catch (const std::logic_error&) { duplicate_owner_rejected = true; }
    CHECK(duplicate_owner_rejected);
    CHECK(old_group.mark_ready(old_actor));
    Inbox inbox({2, 16}, {2, 16}, {2, 16});
    inbox.bind_target(old_actor);
    CHECK(inbox.offer_to(old_actor, Lane::Normal, 5, 8));
    // Physical finalization cannot outrun queued resource ownership.
    CHECK(!old_group.finalized(old_actor));
    const auto finalizer = old_group.runtime_finalizer(old_actor);
    old_group.close();
    CHECK(inbox.retire_queued_after_quiescence(finalizer, old_actor) == 1);
    CHECK(finalizer.finalized());
    old_actor.get_done().get();
    auto new_actor = newer_group.adopt(750, 9, 2, [] {});
    CHECK(newer_group.mark_ready(new_actor));
    int rejected_payload = 17;
    bool rejected = false;
    try {
        (void)inbox.offer_to(new_actor, Lane::Urgent,
                             std::move(rejected_payload), 8);
    } catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected && rejected_payload == 17);
    CHECK(inbox.dispatch_next(newer_group, new_actor, [&](Inbox::Delivery&) {
        CHECK(false && "new generation stole old queued message");
    }) == Inbox::Dispatch::Retired);
    CHECK(inbox.snapshot() == 1 && inbox.used_frames(Lane::Normal) == 0);
    rejected = false;
    try { inbox.bind_target(new_actor); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    CHECK(inbox.dispatch_next(old_group, old_actor, [&](Inbox::Delivery&) {
        CHECK(false && "finalized actor executed a queued user callback");
    }) == Inbox::Dispatch::Retired);
    CHECK(inbox.retire_queued_after_quiescence(finalizer, old_actor) == 0);
    CHECK(inbox.drain_status(1) == Inbox::DrainStatus::Aborted);
    CHECK(newer_group.finalized(new_actor));
    old_actor.get_done().get();
    new_actor.get_done().get();
    inbox.close();
}

// An actor's numeric runtime/id/generation may be reused after physical
// finalization. It must not grant old capability holders access to a new
// incarnation's inbox, even though the routing labels are equal (ABA).
static void same_numeric_identity_cannot_reuse_old_incarnation_ticket() {
    NativeActorGroup old_group(751, 1);
    auto old_actor = old_group.adopt(751, 9, 1, [] {});
    CHECK(old_group.mark_ready(old_actor));
    CHECK(old_group.finalized(old_actor));
    old_actor.get_done().get();

    NativeActorGroup next_group(751, 1);
    auto next_actor = next_group.adopt(751, 9, 1, [] {});
    CHECK(next_group.mark_ready(next_actor));
    CHECK(old_actor == next_actor); // identical numeric routing labels
    CHECK(!old_actor.same_incarnation(next_actor)); // distinct native tickets

    Inbox inbox({2, 16}, {2, 16}, {2, 16});
    inbox.bind_target(next_actor);
    int stale_payload = 404;
    bool rejected = false;
    try { (void)inbox.offer_to(old_actor, Lane::Urgent,
                               std::move(stale_payload), 8); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected && stale_payload == 404);
    CHECK(inbox.snapshot() == 0);
    CHECK(next_actor.outstanding_resource_loans() == 0);

    auto stolen_owner = std::make_unique<int>(17);
    rejected = false;
    try { (void)inbox.offer_owned_to(old_actor, Lane::Normal, stolen_owner, 8); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected && stolen_owner && *stolen_owner == 17);
    CHECK(next_actor.outstanding_resource_loans() == 0);

    CHECK(inbox.offer_to(next_actor, Lane::Normal, 5, 8));
    CHECK(inbox.dispatch_next(next_group, next_actor, [&](Inbox::Delivery& item) {
        CHECK(item.value() == 5 && item.sequence() == 1);
        CHECK(item.acknowledge_released());
    }) == Inbox::Dispatch::User);
    CHECK(inbox.watermark() == 1);
    CHECK(next_actor.outstanding_resource_loans() == 0);
    CHECK(next_group.finalized(next_actor));
    next_actor.get_done().get();
    inbox.close();
}

static void failed_queue_publication_retains_sequence_and_window_credit() {
    oreslang::runtime::ActorMailboxCompletion ledger(2);
    CHECK(ledger.reserve_prepare());
    bool failed = false;
    try {
        (void)ledger.publish([](std::uint64_t) {
            throw std::runtime_error("injected publication failure");
        });
    } catch (const std::runtime_error&) { failed = true; }
    CHECK(failed);
    CHECK(ledger.snapshot() == 0);
    // The failed queue insertion still owns a *preparing* reservation until
    // rollback. Nothing may be considered delivered or completed.
    CHECK(ledger.reserve_prepare());
    CHECK(!ledger.reserve_prepare()); // finite admission window
    ledger.cancel_prepare();
    const auto first = ledger.publish([](std::uint64_t seq) { CHECK(seq == 1); });
    CHECK(first == 1 && ledger.snapshot() == 1);
    CHECK(!ledger.completed_through(1));
    ledger.complete(first);
    CHECK(ledger.completed_through(1));
    bool duplicate = false;
    try { ledger.complete(first); }
    catch (const std::logic_error&) { duplicate = true; }
    CHECK(duplicate);
}

static void global_drain_never_passes_overtaken_normal() {
    Inbox inbox({4, 32}, {4, 32}, {4, 32});
    CHECK(inbox.offer(Lane::Normal, 101, 8));
    const auto first_barrier = inbox.snapshot();
    CHECK(first_barrier == 1);
    CHECK(inbox.offer(Lane::Urgent, 201, 8));
    const auto both_barrier = inbox.snapshot();
    CHECK(both_barrier == 2);
    auto urgent = inbox.try_receive();
    CHECK(urgent && urgent->lane() == Lane::Urgent);
    CHECK(urgent->sequence() == 2);
    CHECK(urgent->acknowledge_released());
    CHECK(inbox.watermark() == 0);
    CHECK(!inbox.completed_through(first_barrier));
    CHECK(!inbox.completed_through(both_barrier));
    auto normal = inbox.try_receive();
    CHECK(normal && normal->lane() == Lane::Normal);
    CHECK(normal->sequence() == 1);
    CHECK(normal->acknowledge_released());
    CHECK(inbox.watermark() == 2);
    CHECK(inbox.completed_through(first_barrier));
    CHECK(inbox.completed_through(both_barrier));
    inbox.close();
}

static void stalled_ack_bounds_global_reordering_and_recovers() {
    // Even if URGENT keeps ACKing, a missing earlier NORMAL must not grow an
    // unbounded sparse completion set. Max 3 unfinished sequence positions.
    Inbox inbox({2, 16}, {2, 16}, {2, 16}, 3);
    CHECK(inbox.offer(Lane::Normal, 100, 8));
    auto held = inbox.try_receive();
    CHECK(held && held->lane() == Lane::Normal && held->sequence() == 1);
    for (int n = 0; n < 2; ++n) {
        CHECK(inbox.offer(Lane::Urgent, int(n), 8));
        auto urgent = inbox.try_receive();
        CHECK(urgent && urgent->lane() == Lane::Urgent);
        CHECK(urgent->sequence() == static_cast<std::uint64_t>(n + 2));
        CHECK(urgent->acknowledge_released());
    }
    CHECK(inbox.watermark() == 0 && inbox.snapshot() == 3);
    int refused = 42;
    CHECK(!inbox.offer(Lane::Bulk, std::move(refused), 8));
    CHECK(refused == 42); // denied before move, including global window denial
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    CHECK(held->acknowledge_released());
    CHECK(inbox.watermark() == 3);
    CHECK(inbox.offer(Lane::Bulk, 999, 8));
    CHECK(inbox.snapshot() == 4);
    auto bulk = inbox.try_receive();
    CHECK(bulk && bulk->sequence() == 4);
    CHECK(bulk->acknowledge_released());
    CHECK(inbox.completed_through(4));
    inbox.close();
}

struct FailOnceMove {
    static inline bool fail_move = false;
    int payload;
    explicit FailOnceMove(int v) : payload(v) {}
    FailOnceMove(FailOnceMove&& other) {
        if (fail_move) throw std::runtime_error("injected move fault");
        payload = other.payload;
    }
    FailOnceMove(const FailOnceMove&) = delete;
    FailOnceMove& operator=(const FailOnceMove&) = delete;
};

static void generation_loans_refund_denied_and_throwing_admissions() {
    NativeActorGroup group(8894, 1);
    auto actor = group.adopt(8894, 22, 1, [] {});
    CHECK(group.mark_ready(actor));
    using TypedInbox = PriorityActorInbox<FailOnceMove>;
    TypedInbox inbox({1, 8}, {1, 8}, {1, 8}, 2);
    inbox.bind_target(actor);
    FailOnceMove payload(55);
    FailOnceMove::fail_move = true;
    bool failed = false;
    try { (void)inbox.offer_to(actor, TypedInbox::Lane::Normal,
                               std::move(payload), 8); }
    catch (const std::runtime_error&) { failed = true; }
    FailOnceMove::fail_move = false;
    CHECK(failed && payload.payload == 55);
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(inbox.snapshot() == 0);
    CHECK(inbox.offer_to(actor, TypedInbox::Lane::Normal,
                         std::move(payload), 8));
    CHECK(actor.outstanding_resource_loans() == 1);
    FailOnceMove rejected(66);
    CHECK(!inbox.offer_to(actor, TypedInbox::Lane::Normal,
                          std::move(rejected), 8));
    CHECK(rejected.payload == 66);
    CHECK(actor.outstanding_resource_loans() == 1);
    bool received = false;
    CHECK(inbox.dispatch_next(group, actor, [&](TypedInbox::Delivery& transferred) {
        CHECK(transferred.value().payload == 55);
        CHECK(transferred.acknowledge_released());
        CHECK(!transferred.acknowledge_released());
        received = true;
    }) == TypedInbox::Dispatch::User);
    CHECK(received);
    CHECK(actor.outstanding_resource_loans() == 0);
    CHECK(group.finalized(actor));
    actor.get_done().get();
    inbox.close();
}

static void failed_admission_never_consumes_global_identity() {
    PriorityActorInbox<FailOnceMove> inbox({2, 16}, {2, 16}, {2, 16}, 2);
    FailOnceMove producer(77);
    FailOnceMove::fail_move = true;
    bool threw = false;
    try { (void)inbox.offer(PriorityActorInbox<FailOnceMove>::Lane::Normal,
                            std::move(producer), 8); }
    catch (const std::runtime_error&) { threw = true; }
    FailOnceMove::fail_move = false;
    CHECK(threw);
    CHECK(producer.payload == 77);
    CHECK(inbox.snapshot() == 0 && inbox.watermark() == 0);
    CHECK(inbox.used_frames(PriorityActorInbox<FailOnceMove>::Lane::Normal) == 0);
    CHECK(inbox.offer(PriorityActorInbox<FailOnceMove>::Lane::Normal,
                      std::move(producer), 8));
    auto accepted = inbox.try_receive();
    CHECK(accepted && accepted->sequence() == 1);
    CHECK(accepted->value().payload == 77);
    CHECK(accepted->acknowledge_released());
    CHECK(inbox.completed_through(1));
    inbox.close();
}

struct PausedPublication {
    std::function<void()> during_move;
    explicit PausedPublication(std::function<void()> callback)
        : during_move(std::move(callback)) {}
    PausedPublication(PausedPublication&& other) noexcept
        : during_move(std::move(other.during_move)) {
        if (during_move) during_move();
    }
    PausedPublication(const PausedPublication&) = delete;
};

static void actor_binding_rejects_unpublished_preparation_and_unguarded_send() {
    NativeActorGroup group(8850, 1);
    auto actor = group.adopt(8850, 43, 1, [] {});
    CHECK(group.mark_ready(actor));
    using PrepInbox = PriorityActorInbox<PausedPublication>;
    using PrepLane = PrepInbox::Lane;
    PrepInbox unbound({1, 8}, {1, 8}, {1, 8}, 3);
    std::promise<void> entered, release;
    auto moved = entered.get_future();
    auto release_gate = release.get_future().share();
    PausedPublication input([&] {
        entered.set_value();
        release_gate.wait(); // TEST ONLY: pause between quota and publication
    });
    std::atomic<bool> published{false};
    std::thread producer([&] {
        published.store(unbound.offer(PrepLane::Normal, std::move(input), 8),
                        std::memory_order_release);
    });
    moved.wait();
    CHECK(unbound.snapshot() == 0);
    CHECK(unbound.used_frames(PrepLane::Normal) == 1);
    bool rejected = false;
    try { unbound.bind_target(actor); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected); // unpublished send is still active admission
    release.set_value();
    producer.join();
    CHECK(published.load(std::memory_order_acquire));
    CHECK(unbound.snapshot() == 1);
    rejected = false;
    try { unbound.bind_target(actor); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected); // binding a previously used inbox is never permitted
    auto old = unbound.try_receive();
    CHECK(old && old->acknowledge_released());
    unbound.close();

    PrepInbox bound({1, 8}, {1, 8}, {1, 8}, 3);
    bound.bind_target(actor);
    PausedPublication plain([] {});
    rejected = false;
    try { (void)bound.offer(PrepLane::Normal, std::move(plain), 8); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected && bound.snapshot() == 0);
    auto owner = std::make_unique<PausedPublication>([] {});
    rejected = false;
    try { (void)bound.offer_owned(PrepLane::Bulk, owner, 8); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected && owner && bound.snapshot() == 0);
    CHECK(bound.offer_to(actor, PrepLane::Normal, std::move(plain), 8));
    CHECK(bound.offer_owned_to(actor, PrepLane::Bulk, owner, 8));
    CHECK(!owner);
    for (int i = 0; i != 2; ++i) {
        CHECK(bound.dispatch_next(group, actor, [&](PrepInbox::Delivery& item) {
            CHECK(item.acknowledge_released());
        }) == PrepInbox::Dispatch::User);
    }
    CHECK(bound.completed_through(2));
    bound.close();

    PrepInbox sealed_empty({1, 8}, {1, 8}, {1, 8}, 2);
    sealed_empty.close();
    rejected = false;
    try { sealed_empty.bind_target(actor); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected && sealed_empty.snapshot() == 0);
    std::atomic<int> unexpected_moves{0};
    PausedPublication retained([&] { ++unexpected_moves; });
    rejected = false;
    try { (void)sealed_empty.offer(PrepLane::Normal, std::move(retained), 8); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected && unexpected_moves.load() == 0);
    CHECK(sealed_empty.used_frames(PrepLane::Normal) == 0);

    CHECK(group.finalized(actor));
    actor.get_done().get();
}

static void stop_cannot_leave_a_preparing_message_unaccounted() {
    using PortInbox = PriorityActorInbox<PausedPublication>;
    using PortLane = PortInbox::Lane;
    PortInbox inbox({1, 8}, {1, 8}, {1, 8}, 3);
    std::promise<void> entered, release;
    auto entered_future = entered.get_future();
    auto release_gate = release.get_future().share();
    PausedPublication source([&] {
        entered.set_value();
        release_gate.wait(); // TEST ONLY: guest actor carriers may never park
    });
    std::atomic<bool> admitted{false};
    std::thread publisher([&] {
        admitted.store(inbox.offer(PortLane::Normal, std::move(source), 8),
                       std::memory_order_release);
    });
    entered_future.wait();
    CHECK(inbox.used_frames(PortLane::Normal) == 1);
    CHECK(inbox.snapshot() == 0); // preparation is not publication
    bool denied_unfenced_retirement = false;
    try { (void)inbox.retire_queued_after_quiescence(); }
    catch (const std::logic_error&) { denied_unfenced_retirement = true; }
    CHECK(denied_unfenced_retirement);
    CHECK(inbox.snapshot() == 0);
    release.set_value();
    publisher.join();
    CHECK(admitted.load(std::memory_order_acquire));
    CHECK(inbox.snapshot() == 1);
    CHECK(inbox.retire_queued_after_quiescence() == 1);
    CHECK(inbox.watermark() == 1);
    CHECK(inbox.drain_status(1) == PortInbox::DrainStatus::Aborted);
    CHECK(inbox.used_frames(PortLane::Normal) == 0);
    inbox.close();
}

static void owned_handle_admission_never_steals_on_rejection_or_constructor_fault() {
    // Even a payload with a deliberately throwing move constructor is safe:
    // the transport only moves unique_ptr (no T constructor under the lock).
    using OwnedInbox = PriorityActorInbox<FailOnceMove>;
    OwnedInbox inbox({1, 8}, {1, 8}, {1, 8}, 2);
    auto first = std::make_unique<FailOnceMove>(42);
    FailOnceMove::fail_move = true;
    CHECK(inbox.offer_owned(OwnedInbox::Lane::Normal, first, 8));
    CHECK(!first);
    auto denied = std::make_unique<FailOnceMove>(99);
    CHECK(!inbox.offer_owned(OwnedInbox::Lane::Normal, denied, 8));
    CHECK(denied && denied->payload == 99);
    auto r = inbox.try_receive();
    CHECK(r && r->sequence() == 1 && r->value().payload == 42);
    CHECK(r->acknowledge_released());
    CHECK(inbox.completed_through(1));
    CHECK(inbox.offer_owned(OwnedInbox::Lane::Bulk, denied, 8));
    CHECK(!denied);
    auto bulk = inbox.try_receive();
    CHECK(bulk && bulk->sequence() == 2 && bulk->value().payload == 99);
    CHECK(bulk->acknowledge_released());
    CHECK(inbox.completed_through(2));
    FailOnceMove::fail_move = false;
    inbox.close();

    // Drain the old incarnation before replacement admission. A restarted
    // actor must not steal the sender's object through the old inbox ticket.
    NativeActorGroup original(751, 1), replacement(751, 1);
    auto original_ref = original.adopt(751, 11, 1, [] {});
    CHECK(original.mark_ready(original_ref));
    Inbox target({1, 8}, {1, 8}, {1, 8});
    target.bind_target(original_ref);
    auto owned = std::make_unique<int>(7);
    CHECK(target.offer_owned_to(original_ref, Lane::Urgent, owned, 8));
    CHECK(!owned);
    CHECK(target.dispatch_next(original, original_ref, [&](Inbox::Delivery& accepted) {
        CHECK(accepted.value() == 7 && accepted.acknowledge_released());
    }) == Inbox::Dispatch::User);
    CHECK(target.completed_through(1));
    CHECK(original.finalized(original_ref));
    original_ref.get_done().get();
    auto replacement_ref = replacement.adopt(751, 11, 2, [] {});
    CHECK(replacement.mark_ready(replacement_ref));
    owned = std::make_unique<int>(17);
    bool invalid = false;
    try { (void)target.offer_owned_to(replacement_ref, Lane::Urgent, owned, 8); }
    catch (const std::logic_error&) { invalid = true; }
    CHECK(invalid && owned && *owned == 17);
    CHECK(target.snapshot() == 1);
    CHECK(replacement.finalized(replacement_ref));
    replacement_ref.get_done().get();
    target.close();
}

static void stop_vs_bound_send_admission_has_one_winner() {
    using SendInbox = PriorityActorInbox<PausedPublication>;
    using SendLane = SendInbox::Lane;
    std::uint32_t send_wins = 0;
    std::uint32_t stop_wins = 0;

    for (std::uint32_t trial = 0; trial < 2000; ++trial) {
        const std::uint64_t runtime = 990000 + trial;
        NativeActorGroup group(runtime, 1);
        std::atomic<int> stops{0};
        auto actor = group.adopt(runtime, 1, 1, [&] {
            stops.fetch_add(1, std::memory_order_acq_rel);
        });
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);

        SendInbox inbox({1, 64}, {1, 64}, {1, 64}, 2);
        inbox.bind_target(actor);
        std::atomic<int> moved{0};
        PausedPublication payload([&] {
            moved.fetch_add(1, std::memory_order_acq_rel);
        });
        std::atomic<int> ready{0};
        bool accepted = false;
        bool rejected = false;

        std::thread sender([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try {
                accepted = inbox.offer_to(
                    actor, SendLane::Normal, std::move(payload), 8);
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        std::thread stopper([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            group.stop_workers();
        });
        sender.join();
        stopper.join();

        CHECK(stops.load(std::memory_order_acquire) == 1);
        CHECK(accepted != rejected);
        CHECK((accepted && moved.load(std::memory_order_acquire) == 1)
              || (rejected && moved.load(std::memory_order_acquire) == 0));

        // STOP is sticky: a second bound send is rejected before moving.
        std::atomic<int> late_moves{0};
        PausedPublication late([&] {
            late_moves.fetch_add(1, std::memory_order_acq_rel);
        });
        bool late_rejected = false;
        try {
            (void)inbox.offer_to(
                actor, SendLane::Urgent, std::move(late), 8);
        } catch (const std::logic_error&) {
            late_rejected = true;
        }
        CHECK(late_rejected);
        CHECK(late_moves.load(std::memory_order_acquire) == 0);

        if (accepted) {
            ++send_wins;
            CHECK(inbox.snapshot() == 1);
            CHECK(actor.outstanding_resource_loans() == 1);
            CHECK(!group.finalized(actor));
        } else {
            ++stop_wins;
            CHECK(actor.outstanding_resource_loans() == 0);
            CHECK(inbox.snapshot() == 0);
        }
        // STOP is not an authorized USER dispatch. The winner's queued
        // message must be retired ABORTED after group/turn quiescence,
        // never stolen by the raw try_receive() API.
        group.close();
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) ==
              (accepted ? 1u : 0u));
        if (accepted) {
            CHECK(inbox.drain_status(1) == SendInbox::DrainStatus::Aborted);
            CHECK(!inbox.completed_through(1));
        }
        CHECK(inbox.used_frames(SendLane::Normal) == 0);
        CHECK(actor.outstanding_resource_loans() == 0);
        CHECK(finalizer.ready_for_resource_reclamation());
        CHECK(finalizer.finalized());
        actor.get_done().get();
        inbox.close();
    }

    CHECK(send_wins + stop_wins == 2000);
}

static void stop_vs_bound_owned_send_preserves_sender_ownership() {
    std::uint32_t send_wins = 0;
    std::uint32_t stop_wins = 0;

    for (std::uint32_t trial = 0; trial < 2000; ++trial) {
        const std::uint64_t runtime = 993000 + trial;
        NativeActorGroup group(runtime, 1);
        std::atomic<int> stops{0};
        auto actor = group.adopt(runtime, 1, 1, [&] {
            stops.fetch_add(1, std::memory_order_acq_rel);
        });
        CHECK(group.mark_ready(actor));
        auto finalizer = group.runtime_finalizer(actor);

        Inbox inbox({1, 64}, {1, 64}, {1, 64}, 2);
        inbox.bind_target(actor);
        auto owner = std::make_unique<int>(static_cast<int>(trial));
        std::atomic<int> ready{0};
        bool accepted = false;
        bool rejected = false;

        std::thread sender([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try {
                accepted = inbox.offer_owned_to(
                    actor, Lane::Bulk, owner, sizeof(int));
            } catch (const std::logic_error&) {
                rejected = true;
            }
        });
        std::thread stopper([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            group.stop_workers();
        });
        sender.join();
        stopper.join();

        CHECK(stops.load(std::memory_order_acquire) == 1);
        CHECK(accepted != rejected);

        if (accepted) {
            ++send_wins;
            CHECK(!owner);
            CHECK(inbox.snapshot() == 1);
            CHECK(actor.outstanding_resource_loans() == 1);
            CHECK(!group.finalized(actor));
        } else {
            ++stop_wins;
            CHECK(owner && *owner == static_cast<int>(trial));
            CHECK(actor.outstanding_resource_loans() == 0);
            CHECK(inbox.snapshot() == 0);
        }

        auto late_owner = std::make_unique<int>(-1);
        bool late_rejected = false;
        try {
            (void)inbox.offer_owned_to(
                actor, Lane::Urgent, late_owner, sizeof(int));
        } catch (const std::logic_error&) {
            late_rejected = true;
        }
        CHECK(late_rejected);
        CHECK(late_owner && *late_owner == -1);

        // Terminal STOP must not process an accepted message as USER:
        // a finalizer-authorized retirement aborts queued work instead.
        group.close();
        CHECK(finalizer.ready_for_actor_turn_quiescence());
        CHECK(inbox.retire_queued_after_quiescence(finalizer, actor) ==
              (accepted ? 1u : 0u));
        if (accepted) {
            CHECK(inbox.drain_status(1) == Inbox::DrainStatus::Aborted);
            CHECK(!inbox.completed_through(1));
        }
        CHECK(inbox.used_frames(Lane::Bulk) == 0);
        CHECK(actor.outstanding_resource_loans() == 0);
        CHECK(finalizer.ready_for_resource_reclamation());
        CHECK(finalizer.finalized());
        actor.get_done().get();
        inbox.close();
    }

    CHECK(send_wins + stop_wins == 2000);
}

static void concurrent_publish_has_unique_global_sequences_and_no_credit_leaks() {
    constexpr int PER_LANE = 500, LANES = 3, TOTAL = PER_LANE * LANES;
    Inbox inbox({16, 128}, {16, 128}, {16, 128}, 64);
    const std::array<Lane, 3> lanes{Lane::Urgent, Lane::Normal, Lane::Bulk};
    std::atomic<int> delivered{0};
    std::array<std::thread, LANES> producers;
    for (int p = 0; p < LANES; ++p) {
        producers[p] = std::thread([&, p] {
            for (int n = 0; n < PER_LANE; ++n) {
                int payload = p * PER_LANE + n;
                while (!inbox.offer(lanes[p], std::move(payload), 8))
                    std::this_thread::yield();
            }
        });
    }
    std::vector<unsigned char> sequences(TOTAL + 1, 0);
    std::array<int, LANES> next_value{0, PER_LANE, 2 * PER_LANE};
    for (int i = 0; i < TOTAL; ++i) {
        auto message = inbox.try_receive();
        if (!message) { --i; std::this_thread::yield(); continue; }
        const auto sequence = message->sequence();
        CHECK(sequence > 0 && sequence <= TOTAL);
        CHECK(sequences[sequence] == 0);
        sequences[sequence] = 1;
        const auto lane = message->lane();
        const int producer_index = lane == Lane::Urgent ? 0 :
                                   lane == Lane::Normal ? 1 : 2;
        CHECK(message->value() == next_value[producer_index]++);
        CHECK(message->acknowledge_released());
        ++delivered;
    }
    for (auto& producer : producers) producer.join();
    CHECK(delivered == TOTAL);
    CHECK(inbox.snapshot() == TOTAL && inbox.watermark() == TOTAL);
    for (std::size_t n = 1; n < sequences.size(); ++n) CHECK(sequences[n]);
    CHECK(inbox.used_frames(Lane::Urgent) == 0);
    CHECK(inbox.used_frames(Lane::Normal) == 0);
    CHECK(inbox.used_frames(Lane::Bulk) == 0);
    inbox.close();
}

int main() {
    bounded_lanes_and_deterministic_fairness();
    sustained_urgent_flood_cannot_starve_normal_or_bulk();
    seal_rejects_all_lanes_but_keeps_admitted_receipts_drainable();
    supervisor_control_and_actor_single_flight_precede_user_priority();
    actual_native_dispatch_obeys_control_and_logical_actor_turn();
    native_dispatch_does_not_steal_mail_from_active_carrier();
    closed_group_never_reclaims_saturated_inbox_with_active_user_turn();
    physically_checked_out_receipt_blocks_finalization_after_stop();
    terminal_stop_fails_drain_instead_of_reporting_dropped_mail_success();
    terminal_retirement_waits_for_inflight_owner_to_ack();
    stale_generation_cannot_reuse_a_live_native_actor_inbox();
    same_numeric_identity_cannot_reuse_old_incarnation_ticket();
    failed_queue_publication_retains_sequence_and_window_credit();
    global_drain_never_passes_overtaken_normal();
    stalled_ack_bounds_global_reordering_and_recovers();
    generation_loans_refund_denied_and_throwing_admissions();
    failed_admission_never_consumes_global_identity();
    actor_binding_rejects_unpublished_preparation_and_unguarded_send();
    stop_cannot_leave_a_preparing_message_unaccounted();
    owned_handle_admission_never_steals_on_rejection_or_constructor_fault();
    stop_vs_bound_send_admission_has_one_winner();
    stop_vs_bound_owned_send_preserves_sender_ownership();
    concurrent_publish_has_unique_global_sequences_and_no_credit_leaks();
}
