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
        CHECK(false && "actor terminal Throw may never reach supervisor on_throw");
        return next;
    }
    SupervisorControlTurn::Decision on_raise(const SupervisorFailureLane::Failure&) override {
        CHECK(false && "actor terminal Raise may never reach supervisor on_raise");
        return next;
    }
    SupervisorControlTurn::Decision on_panic(const SupervisorFailureLane::Failure& f) override {
        CHECK(f.summary == "panic" || f.summary == "throw" || f.summary == "raise");
        if (f.summary == "throw") {
            CHECK(f.origin_kind == SupervisorFailureLane::Kind::Throw);
            CHECK(f.origin_code == 41u);
        }
        if (f.summary == "raise") {
            CHECK(f.origin_kind == SupervisorFailureLane::Kind::Raise);
            CHECK(f.origin_code == 42u);
        }
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
    CHECK(lane.publish_child(second, Kind::Throw, "throw", "parent",
                             std::nullopt, 41u));
    CHECK(!lane.publish_child(first, Kind::Panic, "duplicate"));
    CHECK(lane.urgent());
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.order == std::vector<std::string>({"panic"}));
    CHECK(lane.urgent());
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.order == std::vector<std::string>({"panic", "panic"}));
    CHECK(!lane.urgent());
    // A forged origin mismatching a raw child terminal must fail closed.
    lane.reserve_child(Key{"invalid-origin", 7});
    bool bad_origin = false;
    try {
        (void)lane.publish_child(Key{"invalid-origin", 7},
                Kind::Throw, "throw", "parent", Kind::Raise, 41u);
    } catch (const std::invalid_argument&) { bad_origin = true; }
    CHECK(bad_origin && !lane.urgent());
    lane.retire_child(Key{"invalid-origin", 7});
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

    // A forged decision must escalate once rather than silently
    // acknowledge or loop indefinitely on an unrecognizable host enum.
    Key invalid_decision{"invalid-decision", 5};
    lane.reserve_child(invalid_decision);
    CHECK(lane.publish_child(invalid_decision, Kind::Panic, "panic"));
    hooks.next = static_cast<SupervisorControlTurn::Decision>(0xff);
    const auto escalations_before = hooks.escalations;
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.escalations == escalations_before + 1);
    CHECK(!lane.urgent());
    CHECK(lane.reserved_count() == 0);
    return 0;
}
