#include "oreslang/runtime/EffectSupervisorBridge.hpp"
#include "oreslang/runtime/ActorLocalFailurePolicy.hpp"

#include <stdexcept>
#include <string>

#define CHECK(p) do { if (!(p)) throw std::runtime_error("failed: " #p); } while (false)

namespace {
using oreslang::EffectKind;
using oreslang::EffectStatus;
using oreslang::EffectSupervisorBridge;
using oreslang::EffectUnwind;
using oreslang::SupervisorControlTurn;
using oreslang::SupervisorFailureLane;

EffectStatus return_effect(void* value) noexcept {
    return *static_cast<EffectStatus*>(value);
}

// Standalone host-policy hooks, not emitted Oreslang guest code.
oreslang::ActorLocalHookReply acknowledge_throw(void* value, EffectStatus) noexcept {
    ++*static_cast<int*>(value);
    return {oreslang::ActorLocalResolution::Handled, {EffectKind::Return, 0}};
}

oreslang::ActorLocalHookReply decline_throw(void* value, EffectStatus) noexcept {
    ++*static_cast<int*>(value);
    return {oreslang::ActorLocalResolution::Escalate, {EffectKind::Return, 0}};
}

oreslang::ActorLocalHookReply panic_in_throw_handler(void* value, EffectStatus) noexcept {
    ++*static_cast<int*>(value);
    return {oreslang::ActorLocalResolution::Handled, {EffectKind::Panic, 99}};
}

struct Hooks final : SupervisorControlTurn::Hooks {
    int callbacks = 0;
    int escalations = 0;
    int user_turns = 0;
    SupervisorFailureLane::Kind last = SupervisorFailureLane::Kind::Throw;
    SupervisorFailureLane::Kind last_origin = SupervisorFailureLane::Kind::Throw;
    std::uint32_t last_code = 0;
    SupervisorControlTurn::Decision decision = SupervisorControlTurn::Decision::Acknowledge;

    SupervisorControlTurn::Decision record(
        SupervisorFailureLane::Kind kind,
        const SupervisorFailureLane::Failure& failure) {
        ++callbacks;
        last = kind;
        last_origin = failure.origin_kind;
        last_code = failure.origin_code;
        return decision;
    }

    SupervisorControlTurn::Decision on_throw(
        const SupervisorFailureLane::Failure& failure) override {
        return record(SupervisorFailureLane::Kind::Throw, failure);
    }
    SupervisorControlTurn::Decision on_raise(
        const SupervisorFailureLane::Failure& failure) override {
        return record(SupervisorFailureLane::Kind::Raise, failure);
    }
    SupervisorControlTurn::Decision on_panic(
        const SupervisorFailureLane::Failure& failure) override {
        return record(SupervisorFailureLane::Kind::Panic, failure);
    }
    SupervisorControlTurn::Decision on_cancel(
        const SupervisorFailureLane::Failure& failure) override {
        return record(SupervisorFailureLane::Kind::Cancel, failure);
    }
    SupervisorControlTurn::Decision on_host_failure(
        const SupervisorFailureLane::Failure& failure) override {
        return record(SupervisorFailureLane::Kind::HostFailure, failure);
    }
    void on_escalate(const SupervisorFailureLane::Failure&) override {
        ++escalations;
    }
    bool on_restart(const SupervisorFailureLane::Failure&) override {
        throw std::logic_error("unexpected restart");
    }
    void on_user_turn() override { ++user_turns; }
};
}

int main() {
    SupervisorControlTurn parent(1, 0);
    Hooks hooks;
    using Key = SupervisorFailureLane::ActorKey;
    using Admitted = SupervisorControlTurn::Admitted;

    // An ordinary source throw followed by a deferred panic is fatal to the
    // child. Native unwind LIFO evaluation happens before CONTROL publication.
    const Key panicking{"child-panic", 1};
    parent.reserve_child(panicking);
    EffectStatus deferred_panic{EffectKind::Panic, 99};
    EffectStatus deferred_return{EffectKind::Return, 0};
    EffectUnwind<2> frame;
    CHECK(frame.push(return_effect, &deferred_return));
    CHECK(frame.push(return_effect, &deferred_panic));
    const auto unwind = frame.finish({EffectKind::Throw, 3});
    CHECK(unwind.primary.kind == EffectKind::Panic);
    CHECK(unwind.secondary_count == 1);
    CHECK(unwind.secondary[0].kind == EffectKind::Throw);
    CHECK(!oreslang::native_trap_admits(unwind.primary.kind));
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, panicking, unwind.primary, "terminal deferred panic"));
    CHECK(parent.pending_count() == 1);
    CHECK(parent.urgent());
    CHECK(hooks.callbacks == 0); // child cannot execute parent handlers
    CHECK(parent.admit_turn(hooks, true) == Admitted::Control);
    CHECK(hooks.callbacks == 1);
    CHECK(hooks.last == SupervisorFailureLane::Kind::Panic);
    CHECK(hooks.last_origin == SupervisorFailureLane::Kind::Panic);
    CHECK(hooks.last_code == 99);
    CHECK(hooks.user_turns == 0); // CONTROL preempts user mailbox work
    CHECK(!parent.urgent() && parent.reserved_count() == 0);

    // Forged native tags must not strand a reserved child or enter trap.
    const Key invalid{"child-invalid", 2};
    parent.reserve_child(invalid);
    const auto forged = static_cast<EffectKind>(0xff);
    CHECK(!oreslang::native_trap_admits(forged));
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, invalid, {forged, 0}, "forged success"));
    CHECK(!EffectSupervisorBridge::publish_terminal(
        parent, invalid, {forged, 0}, "duplicate"));
    CHECK(hooks.callbacks == 1);
    CHECK(parent.admit_turn(hooks, true) == Admitted::Control);
    CHECK(hooks.last == SupervisorFailureLane::Kind::HostFailure);
    CHECK(parent.reserved_count() == 0);

    // A corrupted hook decision cannot silently acknowledge a child failure.
    const Key invalid_decision{"child-invalid-decision", 3};
    parent.reserve_child(invalid_decision);
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, invalid_decision, {EffectKind::Throw, 1}, "throw"));
    hooks.decision = static_cast<SupervisorControlTurn::Decision>(0xff);
    CHECK(parent.admit_turn(hooks, true) == Admitted::Control);
    CHECK(hooks.last == SupervisorFailureLane::Kind::Panic);
    CHECK(hooks.last_origin == SupervisorFailureLane::Kind::Throw);
    CHECK(hooks.last_code == 1);
    CHECK(hooks.escalations == 1);
    CHECK(parent.restarts_used() == 0);
    CHECK(parent.reserved_count() == 0);

    // An ordinary unhandled Raise is delivered as actor-terminal Panic,
    // preserving the original typed supervisor CONTROL provenance.
    hooks.decision = SupervisorControlTurn::Decision::Acknowledge;
    const Key raised{"child-unhandled-raise", 17};
    parent.reserve_child(raised);
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, raised, {EffectKind::Raise, 44}, "raised"));
    CHECK(parent.urgent() && parent.pending_count() == 1);
    const int before_raised = hooks.callbacks;
    CHECK(parent.admit_turn(hooks, true) == Admitted::Control);
    CHECK(hooks.callbacks == before_raised + 1);
    CHECK(hooks.last == SupervisorFailureLane::Kind::Panic);
    CHECK(hooks.last_origin == SupervisorFailureLane::Kind::Raise);
    CHECK(hooks.last_code == 44);
    CHECK(!parent.urgent() && parent.reserved_count() == 0);

    // An actor ProcessExit(0) is process control, not a successful child
    // termination; it becomes an explicit host-failure CONTROL message.
    hooks.decision = SupervisorControlTurn::Decision::Acknowledge;
    const Key exit{"child-process-exit", 4};
    parent.reserve_child(exit);
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, exit, {EffectKind::ProcessExit, 0}, "exit"));
    CHECK(parent.admit_turn(hooks, false) == Admitted::Control);
    CHECK(hooks.last == SupervisorFailureLane::Kind::HostFailure);
    CHECK(parent.reserved_count() == 0);

    // A normal return does not publish a failure; lifecycle owner retires it.
    const Key clean{"child-success", 5};
    parent.reserve_child(clean);
    CHECK(!EffectSupervisorBridge::publish_terminal(
        parent, clean, {EffectKind::Return, 0}, "ok"));
    CHECK(parent.pending_count() == 0);
    parent.retire_child(clean);
    CHECK(parent.admit_turn(hooks, false) == Admitted::None);
    CHECK(parent.reserved_count() == 0);

    // The local hook executes on the failing actor before the supervisor
    // terminal bridge. A handled throw produces no parent CONTROL event.
    int local_calls = 0;
    const Key handled{"child-locally-handled", 6};
    parent.reserve_child(handled);
    auto local = oreslang::dispatch_actor_local_unhandled(
        {EffectKind::Throw, 61}, &local_calls, acknowledge_throw, nullptr);
    CHECK(local.hook_invoked && local.handled);
    CHECK(local.outgoing.kind == EffectKind::Return);
    CHECK(!EffectSupervisorBridge::publish_terminal(
        parent, handled, local.outgoing, "handled in owning actor"));
    parent.retire_child(handled);
    CHECK(local_calls == 1 && !parent.urgent());

    // A callback that declines cannot accidentally turn the original
    // failure into success simply because the callback returned normally.
    const Key declined{"child-declined", 7};
    parent.reserve_child(declined);
    local = oreslang::dispatch_actor_local_unhandled(
        {EffectKind::Throw, 62}, &local_calls, decline_throw, nullptr);
    CHECK(local.hook_invoked && !local.handled);
    CHECK(local.outgoing.kind == EffectKind::Throw && local.outgoing.code == 62);
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, declined, local.outgoing, "declined guest throw"));
    CHECK(parent.admit_turn(hooks, false) == Admitted::Control);
    CHECK(hooks.last == SupervisorFailureLane::Kind::Panic);
    CHECK(hooks.last_origin == SupervisorFailureLane::Kind::Throw);
    CHECK(hooks.last_code == 62);

    // PANIC cannot be consumed by on_throw, including one returning Handled.
    const Key bypass{"child-panicked", 8};
    parent.reserve_child(bypass);
    const int before_bypass = local_calls;
    local = oreslang::dispatch_actor_local_unhandled(
        {EffectKind::Panic, 63}, &local_calls, acknowledge_throw, nullptr);
    CHECK(!local.hook_invoked && !local.handled);
    CHECK(local_calls == before_bypass);
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, bypass, local.outgoing, "direct panic"));
    CHECK(parent.admit_turn(hooks, false) == Admitted::Control);
    CHECK(hooks.last_origin == SupervisorFailureLane::Kind::Panic);
    CHECK(hooks.last_code == 63);

    // A callback panic is terminal even when the callback claims Handled.
    const Key callback_panic{"child-callback-panic", 9};
    parent.reserve_child(callback_panic);
    local = oreslang::dispatch_actor_local_unhandled(
        {EffectKind::Throw, 64}, &local_calls, panic_in_throw_handler, nullptr);
    CHECK(local.hook_invoked && !local.handled);
    CHECK(local.outgoing.kind == EffectKind::Panic && local.outgoing.code == 99);
    CHECK(EffectSupervisorBridge::publish_terminal(
        parent, callback_panic, local.outgoing, "callback panic"));
    CHECK(parent.admit_turn(hooks, false) == Admitted::Control);
    CHECK(hooks.last == SupervisorFailureLane::Kind::Panic);
    CHECK(hooks.last_code == 99);
    CHECK(parent.reserved_count() == 0);
}
