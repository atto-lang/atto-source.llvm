#include "oreslang/runtime/PriorityActorInbox.hpp"
#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/SupervisorControlTurn.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>

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
    for (int i = 0; i < 16; ++i) CHECK(inbox.offer(Lane::Urgent, i, 8));
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

int main() {
    bounded_lanes_and_deterministic_fairness();
    sustained_urgent_flood_cannot_starve_normal_or_bulk();
    seal_rejects_all_lanes_but_keeps_admitted_receipts_drainable();
    supervisor_control_and_actor_single_flight_precede_user_priority();
}
