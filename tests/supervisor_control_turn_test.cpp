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
    bool throw_after_restart = false;
    bool throw_after_escalation = false;
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
        if (throw_after_escalation) {
            throw_after_escalation = false;
            throw std::logic_error("escalation side effect then failure");
        }
    }
    bool on_restart(const SupervisorFailureLane::Failure&) override {
        order.push_back("restart");
        if (throw_after_restart) {
            throw_after_restart = false;
            throw std::logic_error("restart spawned child before failure");
        }
        return restart_ok;
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
    const auto events_before = hooks.order.size();
    CHECK(lane.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.order.size() == events_before + 2);
    CHECK(hooks.order.back() == "escalate");
    CHECK(!lane.urgent());
    CHECK(lane.reserved_count() == 0);

    // A restart callback may create a replacement, then fail before returning.
    // The failed attempt must escalate and must never replay this generation.
    SupervisorControlTurn uncertain_restart(1, 1);
    Key failed_restart{"uncertain-restart", 17};
    uncertain_restart.reserve_child(failed_restart);
    CHECK(uncertain_restart.publish_child(failed_restart, Kind::Panic, "panic"));
    Hooks restarted;
    restarted.next = SupervisorControlTurn::Decision::Restart;
    restarted.throw_after_restart = true;
    bool restart_threw = false;
    try { (void) uncertain_restart.admit_turn(restarted, false); }
    catch (const std::logic_error&) { restart_threw = true; }
    CHECK(restart_threw);
    CHECK(restarted.order == std::vector<std::string>({"panic", "restart", "escalate"}));
    CHECK(uncertain_restart.restarts_used() == 1);
    CHECK(!uncertain_restart.urgent() && uncertain_restart.reserved_count() == 0);
    CHECK(uncertain_restart.admit_turn(restarted, false)
          == SupervisorControlTurn::Admitted::None);

    // Escalation may also emit a terminal host event and then throw; retrying
    // would duplicate that irreversible action, so the notice is consumed.
    SupervisorControlTurn uncertain_escalation(1, 0);
    Key failed_escalation{"uncertain-escalation", 18};
    uncertain_escalation.reserve_child(failed_escalation);
    CHECK(uncertain_escalation.publish_child(failed_escalation, Kind::Panic, "panic"));
    Hooks escalated;
    escalated.next = SupervisorControlTurn::Decision::Escalate;
    escalated.throw_after_escalation = true;
    bool escalation_threw = false;
    try { (void) uncertain_escalation.admit_turn(escalated, false); }
    catch (const std::logic_error&) { escalation_threw = true; }
    CHECK(escalation_threw);
    CHECK(escalated.order == std::vector<std::string>({"panic", "escalate"}));
    CHECK(!uncertain_escalation.urgent()
          && uncertain_escalation.reserved_count() == 0);
    CHECK(uncertain_escalation.admit_turn(escalated, false)
          == SupervisorControlTurn::Admitted::None);

    return 0;
}
