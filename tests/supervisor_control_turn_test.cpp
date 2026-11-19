#include "oreslang/runtime/SupervisorControlTurn.hpp"
#include <atomic>
#include <exception>
#include <limits>
#include <initializer_list>
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
    bool stop_throws = false;
    int stop_calls = 0;
    SupervisorControlTurn::StopReceipt stop_receipt =
            SupervisorControlTurn::StopReceipt::Acknowledged;
    std::atomic<bool> hold_stop_callback{false};
    std::atomic<bool> stop_entered{false};
    std::atomic<bool> release_stop_callback{false};
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
    SupervisorControlTurn::StopReceipt on_stop(
            const SupervisorFailureLane::Failure&) override {
        order.push_back("stop");
        ++stop_calls;
        if (hold_stop_callback.load(std::memory_order_acquire)) {
            stop_entered.store(true, std::memory_order_release);
            while (!release_stop_callback.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
        if (stop_throws) throw std::runtime_error("ambiguous host STOP");
        return stop_receipt;
    }
    void on_user_turn() override { order.push_back("user"); }
};

int main() {
    // A malicious or corrupted host policy must not allocate an unbounded
    // restart-history vector before validating its configuration.
    for (const auto invalidBudget : {std::size_t{1025},
                 std::numeric_limits<std::size_t>::max()}) {
        bool rejected = false;
        try { SupervisorControlTurn invalid(1, invalidBudget, 10); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected && "restart policy must be bounded before allocation");
    }
    // Zero disables restarts; 1024 is the maximum accepted history.
    SupervisorControlTurn maxHistory(1, 1024, 10);
    CHECK(maxHistory.restarts_used() == 0);

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

    // #175 restart intensity: two attempts may occur inside a 10-tick
    // window, the third must escalate, and capacity returns only after the
    // oldest attempt ages out. No timer/thread is owned by CONTROL.
    SupervisorControlTurn windowed(4, 2, 10);
    Hooks windowHooks;
    windowHooks.next = SupervisorControlTurn::Decision::Restart;
    Key w1{"window-1", 1}, w2{"window-2", 1},
        w3{"window-3", 1}, w4{"window-4", 1};
    windowed.reserve_child(w1);
    windowed.reserve_child(w2);
    windowed.reserve_child(w3);
    windowed.reserve_child(w4);

    CHECK(windowed.publish_child(w1, Kind::Panic, "panic"));
    CHECK(windowed.admit_turn(windowHooks, false, 100)
            == SupervisorControlTurn::Admitted::Control);
    CHECK(windowHooks.order.back() == "restart");
    CHECK(windowed.restarts_in_window() == 1);

    CHECK(windowed.publish_child(w2, Kind::Panic, "panic"));
    CHECK(windowed.admit_turn(windowHooks, false, 105)
            == SupervisorControlTurn::Admitted::Control);
    CHECK(windowHooks.order.back() == "restart");
    CHECK(windowed.restarts_in_window() == 2);

    CHECK(windowed.publish_child(w3, Kind::Panic, "panic"));
    CHECK(windowed.admit_turn(windowHooks, false, 106)
            == SupervisorControlTurn::Admitted::Control);
    CHECK(windowHooks.order.back() == "escalate");
    CHECK(windowed.restarts_in_window() == 2);

    CHECK(windowed.publish_child(w4, Kind::Panic, "panic"));
    CHECK(windowed.admit_turn(windowHooks, false, 111)
            == SupervisorControlTurn::Admitted::Control);
    CHECK(windowHooks.order.back() == "restart");
    CHECK(windowed.restarts_in_window() == 2);
    CHECK(windowed.restarts_used() == 3);

    // A regressing scheduler tick is untrusted evidence: deny restart and
    // escalate without throwing/requeueing the CONTROL event forever.
    SupervisorControlTurn regressing(1, 1, 10);
    Hooks regressionHooks;
    regressionHooks.next = SupervisorControlTurn::Decision::Restart;
    Key r1{"regression-1", 1};
    regressing.reserve_child(r1);
    CHECK(regressing.publish_child(r1, Kind::Panic, "panic"));
    CHECK(regressing.admit_turn(regressionHooks, false, 50)
            == SupervisorControlTurn::Admitted::Control);
    Key r2{"regression-2", 1};
    regressing.reserve_child(r2);
    CHECK(regressing.publish_child(r2, Kind::Panic, "panic"));
    CHECK(regressing.admit_turn(regressionHooks, false, 49)
            == SupervisorControlTurn::Admitted::Control);
    CHECK(regressionHooks.order.back() == "escalate");

    // An acknowledged host STOP is committed only after an explicit receipt.
    SupervisorControlTurn stopping(2);
    Hooks stopHooks;
    stopHooks.next = SupervisorControlTurn::Decision::Stop;
    Key stopped{"stopped", 8};
    stopping.reserve_child(stopped);
    CHECK(stopping.publish_child(stopped, Kind::Panic, "panic"));
    CHECK(stopping.admit_turn(stopHooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(stopHooks.stop_calls == 1);
    CHECK(!stopping.stop_quarantined());
    CHECK(stopping.reserved_count() == 0);
    CHECK(stopping.admit_turn(stopHooks, false) == SupervisorControlTurn::Admitted::None);

    // An uncertain STOP holds the original child generation in-flight and
    // blocks all subsequent CONTROL or user turns until trusted reconciliation.
    SupervisorControlTurn uncertain(2);
    Hooks uncertainHooks;
    uncertainHooks.next = SupervisorControlTurn::Decision::Stop;
    uncertainHooks.stop_receipt = SupervisorControlTurn::StopReceipt::Uncertain;
    Key undecided{"undecided", 9}, laterStop{"another-child", 1};
    uncertain.reserve_child(undecided);
    uncertain.reserve_child(laterStop);
    CHECK(uncertain.publish_child(undecided, Kind::Panic, "panic"));
    CHECK(uncertain.admit_turn(uncertainHooks, false)
            == SupervisorControlTurn::Admitted::Control);
    CHECK(uncertain.stop_quarantined());
    CHECK(uncertain.reserved_count() == 2);
    CHECK(uncertain.pending_count() == 0);
    CHECK(uncertain.publish_child(laterStop, Kind::Panic, "panic"));
    bool deniedStop = false;
    try { (void)uncertain.admit_turn(uncertainHooks, true); }
    catch (const std::logic_error&) { deniedStop = true; }
    CHECK(deniedStop && uncertainHooks.stop_calls == 1);
    deniedStop = false;
    try { uncertain.confirm_stopped(Key{"undecided", 8}); }
    catch (const std::logic_error&) { deniedStop = true; }
    CHECK(deniedStop && uncertain.stop_quarantined());
    uncertain.confirm_stopped(undecided);
    CHECK(!uncertain.stop_quarantined());
    CHECK(uncertain.reserved_count() == 1);
    uncertainHooks.next = SupervisorControlTurn::Decision::Acknowledge;
    CHECK(uncertain.admit_turn(uncertainHooks, false)
            == SupervisorControlTurn::Admitted::Control);
    CHECK(uncertain.reserved_count() == 0);
    deniedStop = false;
    try { uncertain.confirm_stopped(undecided); }
    catch (const std::logic_error&) { deniedStop = true; }
    CHECK(deniedStop);

    // A STOP callback that throws after a possibly applied effect must never
    // be retried automatically. The host must reconcile its exact generation.
    SupervisorControlTurn throwingStop(1);
    Hooks throwingHooks;
    throwingHooks.next = SupervisorControlTurn::Decision::Stop;
    throwingHooks.stop_throws = true;
    Key unknownReceipt{"unknown-receipt", 12};
    throwingStop.reserve_child(unknownReceipt);
    CHECK(throwingStop.publish_child(unknownReceipt, Kind::Panic, "panic"));
    bool ambiguous = false;
    try { (void)throwingStop.admit_turn(throwingHooks, false); }
    catch (const std::runtime_error&) { ambiguous = true; }
    CHECK(ambiguous && throwingStop.stop_quarantined());
    CHECK(throwingStop.reserved_count() == 1);
    CHECK(throwingStop.pending_count() == 0);
    deniedStop = false;
    try { (void)throwingStop.admit_turn(throwingHooks, false); }
    catch (const std::logic_error&) { deniedStop = true; }
    CHECK(deniedStop && throwingHooks.stop_calls == 1);
    throwingStop.confirm_stopped(unknownReceipt);
    CHECK(throwingStop.reserved_count() == 0);

    // A foreign host observer may poll STOP quarantine while the scheduler
    // runs an effectful callback on its CONTROL carrier. This must never
    // read the concurrently mutated stop_pending_ optional (undefined
    // behavior under C++'s data-race rules).
    SupervisorControlTurn concurrentStop(1);
    Hooks held;
    held.next = SupervisorControlTurn::Decision::Stop;
    held.stop_receipt = SupervisorControlTurn::StopReceipt::Uncertain;
    held.hold_stop_callback.store(true, std::memory_order_release);
    Key heldChild{"held-stop", 19};
    concurrentStop.reserve_child(heldChild);
    CHECK(concurrentStop.publish_child(heldChild, Kind::Panic, "panic"));
    std::atomic<bool> turnDone{false};
    std::atomic<bool> turnSucceeded{false};
    std::thread carrier([&] {
        try {
            turnSucceeded.store(
                concurrentStop.admit_turn(held, false)
                    == SupervisorControlTurn::Admitted::Control,
                std::memory_order_release);
        } catch (...) {
            turnSucceeded.store(false, std::memory_order_release);
        }
        turnDone.store(true, std::memory_order_release);
    });
    bool entered = false;
    for (int spin = 0; spin < 200000; ++spin) {
        if (held.stop_entered.load(std::memory_order_acquire)) {
            entered = true;
            break;
        }
        std::this_thread::yield();
    }
    bool quarantineVisible = entered;
    if (entered) {
        for (int n = 0; n < 10000; ++n) {
            if (!concurrentStop.stop_quarantined()) {
                quarantineVisible = false;
                break;
            }
        }
    }
    held.release_stop_callback.store(true, std::memory_order_release);
    carrier.join();
    CHECK(entered);
    CHECK(quarantineVisible);
    CHECK(turnDone.load(std::memory_order_acquire));
    CHECK(turnSucceeded.load(std::memory_order_acquire));
    CHECK(concurrentStop.stop_quarantined());
    concurrentStop.confirm_stopped(heldChild);
    CHECK(!concurrentStop.stop_quarantined());

    return 0;
    // Independent host telemetry threads must be able to sample restart
    // counts while the supervisor's single CONTROL writer mutates them.
    // Without atomic counters this is a C++ data race, even though CONTROL
    // dispatch itself correctly remains single-flight.
    SupervisorControlTurn concurrentMetrics(1, 64, 100);
    Hooks metricsHooks;
    metricsHooks.next = SupervisorControlTurn::Decision::Restart;
    std::atomic<bool> sampling{true};
    std::atomic<bool> invalidMetric{false};
    std::vector<std::thread> metricReaders;
    for (int reader = 0; reader < 4; ++reader) {
        metricReaders.emplace_back([&] {
            std::size_t lastUsed = 0;
            while (sampling.load(std::memory_order_acquire)) {
                const std::size_t used = concurrentMetrics.restarts_used();
                const std::size_t window = concurrentMetrics.restarts_in_window();
                if (used < lastUsed || used > 3000 || window > 64)
                    invalidMetric.store(true, std::memory_order_relaxed);
                lastUsed = used;
            }
        });
    }
    // Even if a CONTROL assertion fails, stop/join the metric readers before
    // propagating the error. Otherwise a joinable std::thread destructor
    // calls terminate() and obscures the actual failing assertion.
    std::exception_ptr writerFailure;
    try {
        for (std::uint64_t i = 1; i <= 3000; ++i) {
            Key child{"metrics-child", i};
            concurrentMetrics.reserve_child(child);
            CHECK(concurrentMetrics.publish_child(child, Kind::Panic, "panic"));
            CHECK(concurrentMetrics.admit_turn(metricsHooks, false, i)
                    == SupervisorControlTurn::Admitted::Control);
        }
    } catch (...) {
        writerFailure = std::current_exception();
    }
    sampling.store(false, std::memory_order_release);
    for (auto& reader : metricReaders) reader.join();
    if (writerFailure) std::rethrow_exception(writerFailure);
    CHECK(!invalidMetric.load(std::memory_order_relaxed));
    CHECK(concurrentMetrics.restarts_used() <= 3000);
    CHECK(concurrentMetrics.restarts_in_window() <= 64);
    CHECK(concurrentMetrics.reserved_count() == 0);


}
