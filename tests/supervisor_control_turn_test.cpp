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
    // Actor reservation is upstream of mailbox publication. A bounded
    // number of slots is not sufficient if each key may have arbitrary
    // size or contain control/noncanonical bytes.
    SupervisorControlTurn identity_lane(1);
    for (const std::string& id : std::vector<std::string>{
             "", std::string(129, 'x'), "child\\nforged",
             std::string("bad\\x7f", 4), std::string(1, char(0xff))}) {
        bool rejected = false;
        try { identity_lane.reserve_child(Key{id, 0}); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        CHECK(identity_lane.reserved_count() == 0);
        CHECK(!identity_lane.urgent());
    }
    const Key largest_allowed{std::string(128, 'x'), 0};
    identity_lane.reserve_child(largest_allowed);
    CHECK(identity_lane.reserved_count() == 1);
    identity_lane.retire_child(largest_allowed);
    CHECK(identity_lane.reserved_count() == 0);

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

    // Even a decision hook can perform host side effects before throwing.
    // Retrying the notice would replay the callback, so escalate once and
    // permanently retire the terminal event while preserving its exception.
    lane.reserve_child(Key{"decision-side-effect", 2});
    CHECK(lane.publish_child(Key{"decision-side-effect", 2}, Kind::Panic, "panic"));
    hooks.fail_once = true;
    const auto decisions_before = hooks.order.size();
    bool threw = false;
    try { (void) lane.admit_turn(hooks, true); }
    catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
    CHECK(hooks.order.size() == decisions_before + 2);
    CHECK(hooks.order.at(decisions_before) == "panic");
    CHECK(hooks.order.at(decisions_before + 1) == "escalate");
    CHECK(!lane.urgent() && lane.pending_count() == 0);
    CHECK(lane.reserved_count() == 0);
    CHECK(lane.admit_turn(hooks, false) == SupervisorControlTurn::Admitted::None);

    // If escalation also fails after recording its effect, the decision
    // error remains the propagated cause, and neither callback is replayed.
    SupervisorControlTurn dual_failure(1, 1);
    Key decision_escalation_failure{"decision-escalation-failure", 9};
    dual_failure.reserve_child(decision_escalation_failure);
    CHECK(dual_failure.publish_child(
            decision_escalation_failure, Kind::Panic, "panic"));
    Hooks twice_failing;
    twice_failing.fail_once = true;
    twice_failing.throw_after_escalation = true;
    bool original_retained = false;
    try { (void)dual_failure.admit_turn(twice_failing, false); }
    catch (const std::logic_error& error) {
        original_retained = std::string(error.what()) == "handler failure";
    }
    CHECK(original_retained);
    CHECK(twice_failing.order == std::vector<std::string>({"panic", "escalate"}));
    CHECK(!dual_failure.urgent() && dual_failure.reserved_count() == 0);
    CHECK(dual_failure.admit_turn(twice_failing, false)
          == SupervisorControlTurn::Admitted::None);

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

    // A restart callback can report failure without throwing, but the
    // subsequent escalation can itself emit a host effect and then throw.
    // Neither operation may be replayed on the same terminal notice.
    SupervisorControlTurn failed_restart_escalation(1, 1);
    Key failed_both{"failed-restart-escalation", 19};
    failed_restart_escalation.reserve_child(failed_both);
    CHECK(failed_restart_escalation.publish_child(
            failed_both, Kind::Panic, "panic"));
    Hooks both;
    both.next = SupervisorControlTurn::Decision::Restart;
    both.restart_ok = false;
    both.throw_after_escalation = true;
    bool both_threw = false;
    try { (void) failed_restart_escalation.admit_turn(both, true); }
    catch (const std::logic_error&) { both_threw = true; }
    CHECK(both_threw);
    CHECK(both.order == std::vector<std::string>({"panic", "restart", "escalate"}));
    CHECK(failed_restart_escalation.restarts_used() == 1);
    CHECK(!failed_restart_escalation.urgent());
    CHECK(failed_restart_escalation.reserved_count() == 0);
    CHECK(failed_restart_escalation.admit_turn(both, false)
          == SupervisorControlTurn::Admitted::None);
    // The logical supervisor turn is single-writer, but monitoring may
    // observe restart intensity concurrently from a separate host thread.
    // All snapshots must be monotonic and never access non-atomic state.
    constexpr std::size_t kConcurrentRestarts = 1024;
    SupervisorControlTurn observed_restarts(1, kConcurrentRestarts);
    Hooks monitor_hooks;
    monitor_hooks.next = SupervisorControlTurn::Decision::Restart;
    std::atomic<bool> reader_stop{false};
    std::atomic<bool> reader_regressed{false};
    std::thread monitor([&] {
        std::size_t prior = 0;
        while (!reader_stop.load(std::memory_order_acquire)) {
            const auto current = observed_restarts.restarts_used();
            if (current < prior || current > kConcurrentRestarts) {
                reader_regressed.store(true, std::memory_order_release);
            }
            prior = current;
            std::this_thread::yield();
        }
    });
    // Never leave a joinable monitor alive if an assertion or host
    // allocation fails: std::thread's destructor would terminate the test
    // process and obscure the actual failing invariant.
    try {
        for (std::size_t i = 0; i < kConcurrentRestarts; ++i) {
            Key child{"monitored-restart-" + std::to_string(i), 1};
            observed_restarts.reserve_child(child);
            CHECK(observed_restarts.publish_child(child, Kind::Panic, "panic"));
            CHECK(observed_restarts.admit_turn(monitor_hooks, false)
                  == SupervisorControlTurn::Admitted::Control);
        }
    } catch (...) {
        reader_stop.store(true, std::memory_order_release);
        monitor.join();
        throw;
    }
    reader_stop.store(true, std::memory_order_release);
    monitor.join();
    CHECK(!reader_regressed.load(std::memory_order_acquire));
    CHECK(observed_restarts.restarts_used() == kConcurrentRestarts);
    CHECK(observed_restarts.reserved_count() == 0);

    return 0;
}
