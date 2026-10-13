#include "oreslang/runtime/EffectSupervisorBridge.hpp"

#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("check failed: " #condition); } while(false)

using oreslang::EffectKind;
using oreslang::EffectStatus;
using Lane = oreslang::SupervisorFailureLane;

struct EventHandler : Lane::Handler {
    std::vector<Lane::Kind> observed;
    void on_throw(const Lane::Failure&) override { observed.push_back(Lane::Kind::Throw); }
    void on_raise(const Lane::Failure&) override { observed.push_back(Lane::Kind::Raise); }
    void on_panic(const Lane::Failure&) override { observed.push_back(Lane::Kind::Panic); }
    void on_cancel(const Lane::Failure&) override { observed.push_back(Lane::Kind::Cancel); }
    void on_host_failure(const Lane::Failure&) override { observed.push_back(Lane::Kind::HostFailure); }
};

struct ProcessExitHooks : oreslang::SupervisorControlTurn::Hooks {
    using Turn = oreslang::SupervisorControlTurn;
    int host_failures = 0;
    Turn::Decision on_throw(const Lane::Failure&) override { return Turn::Decision::Acknowledge; }
    Turn::Decision on_raise(const Lane::Failure&) override { return Turn::Decision::Acknowledge; }
    Turn::Decision on_panic(const Lane::Failure&) override { return Turn::Decision::Acknowledge; }
    Turn::Decision on_cancel(const Lane::Failure&) override { return Turn::Decision::Acknowledge; }
    Turn::Decision on_host_failure(const Lane::Failure&) override {
        ++host_failures;
        return Turn::Decision::Acknowledge;
    }
    void on_escalate(const Lane::Failure&) override {}
    bool on_restart(const Lane::Failure&) override { return false; }
    void on_user_turn() override {}
};

int main() {
    using Bridge = oreslang::EffectSupervisorBridge;
    Lane lane(7);
    auto key = [](int n) -> Lane::ActorKey { return {"worker-" + std::to_string(n), 1}; };
    constexpr EffectKind effects[] = {
        EffectKind::Throw, EffectKind::Raise, EffectKind::Panic,
        EffectKind::Cancel, EffectKind::Fatal
    };
    constexpr Lane::Kind expected[] = {
        Lane::Kind::Throw, Lane::Kind::Raise, Lane::Kind::Panic,
        Lane::Kind::Cancel, Lane::Kind::HostFailure
    };
    // Publication cannot consume user mailbox capacity and never runs guest
    // handlers inline; each pre-reserved slot is published at most once.
    for (int i = 0; i < 5; ++i) {
        lane.reserve(key(i));
        CHECK(Bridge::publish_terminal(lane, key(i), {effects[i], 11u},
                                       "actor failure", "parent"));
        CHECK(!Bridge::publish_terminal(lane, key(i), {effects[i], 12u},
                                        "duplicate", "parent"));
    }
    EventHandler handler;
    for (int i = 0; i < 5; ++i) CHECK(lane.dispatch_one(handler));
    CHECK(handler.observed == std::vector<Lane::Kind>(expected, expected + 5));
    CHECK(lane.reserved_count() == 0);

    // Normal return leaves the reservation intact; lifecycle owner retires it.
    lane.reserve(key(5));
    CHECK(!Bridge::publish_terminal(lane, key(5), {EffectKind::Return, 42u}, "ok"));
    CHECK(lane.pending_count() == 0);
    lane.retire_gracefully(key(5));

    // An actual unwind where a panic in defer wins over a plain throw
    // produces exactly one typed parent CONTROL event.
    lane.reserve(key(6));
    oreslang::EffectUnwind<1> frame;
    const auto cleanup = [](void*) noexcept -> EffectStatus {
        return {EffectKind::Panic, 99u};
    };
    CHECK(frame.push(cleanup, nullptr));
    const auto terminal = frame.finish({EffectKind::Throw, 1u});
    CHECK(terminal.primary.kind == EffectKind::Panic);
    CHECK(terminal.secondary_count == 1);
    CHECK(terminal.secondary[0].kind == EffectKind::Throw);
    CHECK(Bridge::publish_terminal(lane, key(6), terminal.primary, "defer panic"));
    auto delivered = lane.poll();
    CHECK(delivered.has_value());
    CHECK(delivered->kind == Lane::Kind::Panic);
    CHECK(delivered->summary == "defer panic");
    lane.ack(key(6));

    // Every invalid 8-bit native effect tag must fail closed through the
    // reserved supervisor CONTROL slot, not throw C++ across the actor runtime
    // boundary, strand an actor reservation, or preserve forged diagnostics.
    for (unsigned raw = 7; raw < 256; ++raw) {
        const auto actor = key(1000 + static_cast<int>(raw));
        lane.reserve(actor);
        CHECK(Bridge::publish_terminal(lane, actor,
                {static_cast<EffectKind>(raw), 10}, "forged success"));
        CHECK(!Bridge::publish_terminal(lane, actor,
                {static_cast<EffectKind>(raw), 10}, "duplicate"));
        CHECK(lane.pending_count() == 1);
        auto invalid_event = lane.poll();
        CHECK(invalid_event.has_value());
        CHECK(invalid_event->kind == Lane::Kind::HostFailure);
        CHECK(invalid_event->summary == "invalid native effect discriminant");
        lane.ack(actor);
        CHECK(lane.pending_count() == 0);
        CHECK(lane.reserved_count() == 0);
    }

    // ProcessExit is a valid effect but not a graceful actor Return.
    // Route it to the reserved CONTROL failure lane instead of throwing an
    // unknown-effect exception or silently stranding the parent reservation.
    lane.reserve(key(8));
    CHECK(Bridge::publish_terminal(lane, key(8),
                                   {EffectKind::ProcessExit, 0u},
                                   "actor attempted process exit"));
    auto process_exit = lane.poll();
    CHECK(process_exit.has_value());
    CHECK(process_exit->kind == Lane::Kind::HostFailure);
    CHECK(process_exit->summary == "actor attempted process exit");
    lane.ack(key(8));
    CHECK(lane.reserved_count() == 0);

    // Exercise the scheduler-facing overload too. Child publication may only
    // enqueue CONTROL; the parent callback must execute later on its own
    // single-flight scheduler admission, never inline in publish_terminal.
    oreslang::SupervisorControlTurn turn(1);
    const auto child = key(9);
    turn.reserve_child(child);
    ProcessExitHooks hooks;
    CHECK(Bridge::publish_terminal(turn, child,
                                   {EffectKind::ProcessExit, 0u},
                                   "process exit from child"));
    CHECK(hooks.host_failures == 0);
    CHECK(turn.pending_count() == 1);
    CHECK(turn.urgent());
    CHECK(turn.admit_turn(hooks, false) ==
          oreslang::SupervisorControlTurn::Admitted::Control);
    CHECK(hooks.host_failures == 1);
    CHECK(!turn.urgent());
    CHECK(turn.reserved_count() == 0);

    // Test all forged tags through the scheduler-facing overload too.
    // The parent's on_host_failure must execute only during admit_turn.
    for (unsigned raw = 7; raw < 256; ++raw) {
        const auto invalid_child = key(2000 + static_cast<int>(raw));
        turn.reserve_child(invalid_child);
        const int previously_admitted = hooks.host_failures;
        CHECK(Bridge::publish_terminal(turn, invalid_child,
                {static_cast<EffectKind>(raw), 0}, "forged return"));
        CHECK(turn.pending_count() == 1);
        CHECK(turn.urgent());
        CHECK(hooks.host_failures == previously_admitted);
        CHECK(turn.admit_turn(hooks, false) ==
              oreslang::SupervisorControlTurn::Admitted::Control);
        CHECK(hooks.host_failures == previously_admitted + 1);
        CHECK(!turn.urgent());
        CHECK(turn.reserved_count() == 0);
    }
    CHECK(hooks.host_failures == 250);
    return 0;
}
