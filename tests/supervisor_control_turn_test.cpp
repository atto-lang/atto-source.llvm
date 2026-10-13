#include "oreslang/runtime/SupervisorControlTurn.hpp"
#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using oreslang::SupervisorControlTurn;
using oreslang::SupervisorFailureLane;
#define CHECK(ok) do { if (!(ok)) throw std::runtime_error("failed: " #ok); } while (false)

struct Hooks final : SupervisorControlTurn::Hooks {
    std::vector<std::string> order;
    SupervisorControlTurn::Decision next = SupervisorControlTurn::Decision::Acknowledge;
    bool restart_ok = true;
    bool fail_once = false;
    SupervisorControlTurn::Decision on_throw(const SupervisorFailureLane::Failure&) override {
        order.push_back("throw"); return next;
    }
    SupervisorControlTurn::Decision on_raise(const SupervisorFailureLane::Failure&) override {
        order.push_back("raise"); return next;
    }
    SupervisorControlTurn::Decision on_panic(const SupervisorFailureLane::Failure& f) override {
        CHECK(f.summary == "panic");
        order.push_back("panic");
        if (fail_once) { fail_once = false; throw std::logic_error("handler failure"); }
        return next;
    }
    SupervisorControlTurn::Decision on_cancel(const SupervisorFailureLane::Failure&) override {
        order.push_back("cancel"); return next;
    }
    SupervisorControlTurn::Decision on_host_failure(const SupervisorFailureLane::Failure&) override {
        order.push_back("host"); return next;
    }
    void on_escalate(const SupervisorFailureLane::Failure&) override {
        order.push_back("escalate");
    }
    bool on_restart(const SupervisorFailureLane::Failure&) override {
        order.push_back("restart"); return restart_ok;
    }
    void on_user_turn() override { order.push_back("user"); }
};

int main() {
    using Kind = SupervisorFailureLane::Kind;
    using Key = SupervisorFailureLane::ActorKey;
    SupervisorControlTurn lane(3, 1);
    Hooks hooks;
    Key first{"child-one", 0}, second{"child-two", 0}, third{"child-three", 0};
    lane.reserve_child(first);
    lane.reserve_child(second);
    lane.reserve_child(third);

    CHECK(lane.publish_child(first, Kind::Panic, "panic", "parent"));
    CHECK(lane.publish_child(second, Kind::Throw, "throw", "parent"));
    CHECK(!lane.publish_child(first, Kind::Panic, "duplicate"));
    CHECK(lane.urgent());
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.order == std::vector<std::string>({"panic"}));
    CHECK(lane.urgent());
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.order == std::vector<std::string>({"panic", "throw"}));
    CHECK(!lane.urgent());
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::User);
    CHECK(hooks.order.back() == "user");

    hooks.next = SupervisorControlTurn::Decision::Restart;
    CHECK(lane.publish_child(third, Kind::Panic, "panic"));
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.order.back() == "restart");
    CHECK(lane.restarts_used() == 1);

    Key later{"replacement", 1};
    lane.reserve_child(later);
    CHECK(lane.publish_child(later, Kind::Panic, "panic"));
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.order.back() == "escalate");
    CHECK(lane.restarts_used() == 1);
    CHECK(lane.reserved_count() == 0);

    // A failing hook never loses the event; it is retried next admission.
    lane.reserve_child(Key{"retry", 2});
    CHECK(lane.publish_child(Key{"retry", 2}, Kind::Panic, "panic"));
    hooks.fail_once = true;
    bool threw = false;
    try { (void) lane.admit_turn(hooks, true); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
    CHECK(lane.urgent());
    CHECK(lane.pending_count() == 1);
    hooks.next = SupervisorControlTurn::Decision::Acknowledge;
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(lane.reserved_count() == 0);
    CHECK(lane.admit_turn(hooks, false) == SupervisorControlTurn::Admitted::None);

    // After an actor turn, a new producer must make CONTROL ready.
    Key handoff{"handoff", 4};
    lane.reserve_child(handoff);
    CHECK(lane.publish_child(handoff, Kind::Panic, "panic"));
    CHECK(lane.pending_count() == 1);
    CHECK(lane.urgent());
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(!lane.urgent());

    // All 253 invalid one-byte decision tags must escalate, never ACK
    // silently or consume the remaining restart budget. Each event must be
    // handled only on the parent's single-flight admitted CONTROL turn.
    const auto restart_count = lane.restarts_used();
    for (unsigned raw = 3; raw < 256; ++raw) {
        const Key forged_decision{"invalid-decision-" + std::to_string(raw), 5};
        lane.reserve_child(forged_decision);
        CHECK(lane.publish_child(forged_decision, Kind::Throw, "invalid decision"));
        hooks.next = static_cast<SupervisorControlTurn::Decision>(raw);
        const auto observed = hooks.order.size();
        CHECK(lane.admit_turn(hooks, false) == SupervisorControlTurn::Admitted::Control);
        CHECK(hooks.order.size() == observed + 2);
        CHECK(hooks.order[observed] == "throw");
        CHECK(hooks.order[observed + 1] == "escalate");
        CHECK(lane.restarts_used() == restart_count);
        CHECK(lane.reserved_count() == 0);
        CHECK(!lane.urgent());
    }
    return 0;
}
