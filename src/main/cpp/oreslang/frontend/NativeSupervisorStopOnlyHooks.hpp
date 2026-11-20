#pragma once

#include "oreslang/frontend/NativeSupervisorStopOnly.hpp"
#include "oreslang/runtime/SupervisorControlTurn.hpp"

#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace oreslang {

/**
 * Host-only installation of an opt-in, verified STOP-only source plan on an
 * existing native supervisor CONTROL lane. This is NOT LLVM guest actor IR,
 * full on_panic lowering or a capability supplied to source actor code.
 * The host owns pre-reservation, physical child retirement, the STOP effect,
 * and independent verification before confirming an uncertain receipt.
 */
class NativeSupervisorStopOnlyHooks final : public SupervisorControlTurn::Hooks {
public:
    using Failure = SupervisorFailureLane::Failure;
    using Decision = SupervisorControlTurn::Decision;
    using StopReceipt = SupervisorControlTurn::StopReceipt;
    using HostStop = std::function<StopReceipt(const Failure&)>;

    NativeSupervisorStopOnlyHooks(NativeSupervisorStopOnlyPlan plan, HostStop host_stop)
        : plan_(std::move(plan)), host_stop_(std::move(host_stop)) {
        if (!host_stop_) throw std::invalid_argument("host STOP receipt sink required");
    }
    const NativeSupervisorStopOnlyPlan& plan() const noexcept { return plan_; }

    Decision on_panic(const Failure& failure) override {
        if (failure.kind != SupervisorFailureLane::Kind::Panic ||
            failure.actor.id.empty() || failure.actor.generation == 0) {
            throw std::logic_error("invalid typed terminal Failure for source STOP");
        }
        // The queued Failure is created by the host CONTROL lane, never by
        // source actor code. Only an admitted parent turn may call this hook.
        active_actor_ = failure.actor;
        return Decision::Stop;
    }
    StopReceipt on_stop(const Failure& failure) override {
        if (!active_actor_ || active_actor_->id != failure.actor.id ||
            active_actor_->generation != failure.actor.generation ||
            failure.kind != SupervisorFailureLane::Kind::Panic) {
            throw std::logic_error("STOP effect without matching on_panic CONTROL authority");
        }
        active_actor_.reset();  // never invoke host effect twice for one hook
        return host_stop_(failure);
    }
    Decision on_throw(const Failure&) override { return unsupported(); }
    Decision on_raise(const Failure&) override { return unsupported(); }
    Decision on_cancel(const Failure&) override { return unsupported(); }
    Decision on_host_failure(const Failure&) override { return unsupported(); }
    void on_escalate(const Failure&) override { (void)unsupported(); }
    bool on_restart(const Failure&) override { (void)unsupported(); return false; }
    void on_user_turn() override { (void)unsupported(); }

private:
    [[noreturn]] static Decision unsupported() {
        throw std::logic_error("restricted source supervisor has no non-STOP effect");
    }
    const NativeSupervisorStopOnlyPlan plan_;
    HostStop host_stop_;
    std::optional<SupervisorFailureLane::ActorKey> active_actor_;
};

} // namespace oreslang
