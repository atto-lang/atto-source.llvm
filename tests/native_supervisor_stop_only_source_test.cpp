#include "oreslang/frontend/NativeSupervisorStopOnlyHooks.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#define CHECK(expr) do { if (!(expr)) throw std::runtime_error("failed: " #expr); } while (false)

namespace {
using oreslang::compile_native_stop_only_supervisor;
using oreslang::NativeSupervisorStopOnlyHooks;
using oreslang::SupervisorControlTurn;
using oreslang::SupervisorFailureLane;

void denied(std::string_view source) {
    bool rejected = false;
    try { (void)compile_native_stop_only_supervisor(source); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
}

int main() {
    constexpr std::string_view source = R"(
        // Explicitly opt-in: general LLVM guest lowering remains unavailable.
        define supervisor Root as
          @Implements
          pub on_panic(problem: Failure): void {
            self.stop(problem);
            return;
          }
        end
    )";
    auto plan = compile_native_stop_only_supervisor(source);
    CHECK(plan.name() == "Root");
    CHECK(plan.explicitly_bound_failure());
    CHECK(plan.target() == oreslang::NativeSupervisorStopOnlyPlan::Target::CurrentFailure);
    const auto implicit = compile_native_stop_only_supervisor(
        "pub define supervisor Main as on_panic(f: Failure) -> void { self.stop(); return; } end");
    CHECK(implicit.name() == "Main" && !implicit.explicitly_bound_failure());
    const auto alternative = compile_native_stop_only_supervisor(
        "define pub supervisor Main as on_panic(f: Failure): void { self.stop(f); return; } end");
    CHECK(alternative.name() == "Main" && alternative.explicitly_bound_failure());
    CHECK(compile_native_stop_only_supervisor(
        "define supervisor S as /* comment { /* nested */ } */ "
        "on_panic(f: Failure): void { self.stop(f); return; } end").name() == "S");

    for (const auto bad : {
        "define supervisor S as on_panic(f: String): void { self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): int { self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(other); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f.child); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f.actor); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f); self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.restart(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { return; self.stop(f); } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f); return; } end define actor X as end",
        "define module Work as define supervisor S as on_panic(f: Failure): void { self.stop(f); return; } end end",
        "define supervisor S as async on_panic(f: Failure): void { await self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { await self.stop(f); return; } end",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f); return; } end /* unterminated",
        "define supervisor S as on_panic(f: Failure): void { self.stop(f); return; } end \"injected\"",
        "pub define pub supervisor S as on_panic(f: Failure): void { self.stop(f); return; } end",
        "define supervisor S as on_panic(self: Failure): void { self.stop(self); return; } end"
    }) denied(bad);
    denied(std::string(1'048'577, ' '));
    denied(std::string("define supervisor S as\0", 23));

    // The source-plan has no authority to publish child Failure itself.
    // Terminal child Throw is delivered as a typed parent Panic with origin.
    int host_calls = 0;
    bool finalized = false;
    NativeSupervisorStopOnlyHooks hooks(std::move(plan), [&](const auto& failure) {
        CHECK(finalized);
        CHECK(failure.actor.id == "child-native" && failure.actor.generation == 7);
        CHECK(failure.kind == SupervisorFailureLane::Kind::Panic);
        CHECK(failure.origin_kind == SupervisorFailureLane::Kind::Throw);
        CHECK(failure.origin_code == 42);
        ++host_calls;
        return SupervisorControlTurn::StopReceipt::Acknowledged;
    });
    SupervisorControlTurn parent(1);
    SupervisorFailureLane::ActorKey child{"child-native", 7};
    parent.reserve_child(child);
    finalized = true; // trusted physical child teardown, not guest assertion
    CHECK(parent.publish_child(child, SupervisorFailureLane::Kind::Throw,
                               "terminal", "parent", std::nullopt, 42));
    CHECK(!parent.publish_child(child, SupervisorFailureLane::Kind::Throw, "duplicate"));
    CHECK(host_calls == 0 && parent.urgent());
    CHECK(parent.admit_turn(hooks, true) == SupervisorControlTurn::Admitted::Control);
    CHECK(host_calls == 1);
    CHECK(parent.reserved_count() == 0 && !parent.stop_quarantined());

    // Ambiguous STOP effects must not be repeated. The trusted owner must
    // independently verify the effect before calling confirm_stopped().
    auto uncertain_plan = compile_native_stop_only_supervisor(source);
    int uncertain_calls = 0;
    NativeSupervisorStopOnlyHooks uncertain_hooks(std::move(uncertain_plan), [&](const auto&) {
        ++uncertain_calls;
        return SupervisorControlTurn::StopReceipt::Uncertain;
    });
    SupervisorControlTurn unresolved(1);
    SupervisorFailureLane::ActorKey pending{"pending", 9};
    unresolved.reserve_child(pending);
    CHECK(unresolved.publish_child(pending, SupervisorFailureLane::Kind::Panic, "panic"));
    CHECK(unresolved.admit_turn(uncertain_hooks, false) == SupervisorControlTurn::Admitted::Control);
    CHECK(uncertain_calls == 1 && unresolved.stop_quarantined());
    bool blocked = false;
    try { (void)unresolved.admit_turn(uncertain_hooks, true); }
    catch (const std::logic_error&) { blocked = true; }
    CHECK(blocked && uncertain_calls == 1 && unresolved.reserved_count() == 1);
    unresolved.confirm_stopped(pending); // only trusted host may prove STOP
    CHECK(!unresolved.stop_quarantined() && unresolved.reserved_count() == 0);
}
