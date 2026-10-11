#include "oreslang/frontend/Parser.hpp"
#include "oreslang/runtime/SourceSupervisorStop.hpp"

#include <deque>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <iostream>

static void check(bool condition, const char* why) {
    if (!condition) throw std::runtime_error(why);
}
static void reject_supervisor(std::string_view source) {
    try {
        (void) oreslang::parse_program(source, false);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        if (message.find("source supervisor on_panic requires typed Failure")
                == std::string::npos
            || message.find("compiler-lowered CONTROL turns")
                == std::string::npos) {
            throw std::runtime_error(
                "source supervisor produced unexpected native diagnostic: " + message);
        }
        return;
    }
    throw std::runtime_error("LLVM admitted general source supervisor without CONTROL lowering");
}

static void reject_stop_slice(std::string_view source) {
    try {
        (void) oreslang::parse_stop_only_supervisor(source);
    } catch (const std::runtime_error&) {
        return;
    }
    throw std::runtime_error("native STOP-only parser admitted unsafe source");
}

int main() {
    reject_supervisor(R"(
        define supervisor Root as
          pub async on_panic(f: Failure): void { return; }
        end
        pub fnc main(): int { return 1; }
    )");
    reject_supervisor(R"(
        define module M as
          define supervisor Root as
            on_panic(f: Failure): void { return; }
          end
        end
    )");

    constexpr std::string_view source = R"(
        define supervisor Root as
          @Implements
          on_panic(f: Failure): void {
            self.stop();
            return;
          }
        end
    )";
    const auto plan = oreslang::parse_stop_only_supervisor(source);
    check(plan.name == "Root" && plan.module.empty(),
          "native STOP source plan identity drift");
    const auto module = oreslang::parse_stop_only_supervisor(R"(
        define module Services as
          define supervisor Worker as
            on_panic(f: Failure): void { self.stop(); return; }
          end
        end
    )");
    check(module.name == "Worker" && module.module == "Services",
          "native STOP plan module identity drift");
    // CONTROL serialization belongs to the runtime. The source hook
    // needs neither a lock declaration nor a special annotation to request
    // the current failure\'s single terminal STOP decision.
    const auto implicit = oreslang::parse_stop_only_supervisor(R"(
        define supervisor Root as
          pub on_panic(f: Failure): void {
            self.stop();
            return;
          }
        end
    )");
    check(implicit.name == "Root", "implicit supervisor CONTROL was not accepted");

    // Explicitly naming the current typed Failure is allowed; it is not
    // an arbitrary child reference or a guest-visible CONTROL lock.
    constexpr std::string_view with_failure = R"(
        define supervisor Root as
          on_panic(problem: Failure): void {
            self.stop(problem);
            return;
          }
        end
    )";
    const auto targeted = oreslang::parse_stop_only_supervisor(with_failure);
    check(targeted.name == "Root" && targeted.explicitly_bound_failure,
          "typed current Failure was not bound to STOP");
    check(!plan.explicitly_bound_failure,
          "zero-argument STOP incorrectly claims an explicit Failure binding");
    constexpr std::string_view with_child = R"(
        define supervisor Root as
          on_panic(problem: Failure): void {
            self.stop(problem.child);
            return;
          }
        end
    )";
    const auto child_plan = oreslang::parse_stop_only_supervisor(with_child);
    check(child_plan.explicitly_bound_failure && child_plan.targets_logical_child,
          "checked Failure.child did not lower to a restricted child target");
    check(!targeted.targets_logical_child,
          "direct typed Failure unexpectedly requires a child projection");
    // The Java opt-in async STOP acknowledgment slice does not imply
    // native async guest lowering. The normal native parser must still reject
    // the identical source until its CONTROL continuation is implemented.
    constexpr std::string_view awaited_stop = R"(
        define supervisor Root as
          @Implements
          pub async on_panic(f: Failure): void {
            await self.stop(f);
            return;
          }
        end
    )";
    reject_supervisor(awaited_stop);
    reject_stop_slice(awaited_stop);
    reject_supervisor(with_child); // normal compiler remains closed
    reject_supervisor(with_failure); // general native compiler remains closed
    for (const auto forged : {
            "self.stop(other); return;",
            "self.stop(self); return;",
            "self.stop(problem.child.actor); return;",
            "self.stop(problem.actor); return;",
            "using self.control { self.stop(); } return;",
            "using supervisor.control { self.stop(); } return;",
            "self.stop(problem, problem); return;",
            "self.stop(1); return;",
            "self.stop(problem); self.stop(problem); return;"
    }) {
        const std::string unsupported =
            "define supervisor Root as on_panic(problem: Failure): void { "
            + std::string(forged) + " } end";
        reject_stop_slice(unsupported);
    }
    reject_supervisor(source); // default compiler remains fail-closed

    for (auto source_text : {
            "define supervisor S as on_panic(f: Failure): void { return; } end",
            "define supervisor S as on_panic(f: String): void { self.stop(); return; } end",
            "define supervisor S as async on_panic(f: Failure): void { self.stop(); return; } end",
            "define supervisor S as on_panic(f: Failure): void { self.restart(); return; } end",
            "define supervisor S as on_panic(f: Failure): void { self.stop(); self.stop(); return; } end",
            "define supervisor S as on_panic(f: Failure): void { print(1); self.stop(); return; } end",
            "define supervisor S as on_panic(f: Failure): void { self.stop(); return 1; } end",
            "define supervisor S as on_panic(f: Failure): void { f.stop(); return; } end",
            "define supervisor S as on_panic(f: Failure): void { self.stop(42); return; } end",
            "define supervisor S as on_panic(f: Failure): void { self.stop(); return; } tear_down(): void { return; } end"
    }) reject_stop_slice(source_text);

    std::deque<std::function<void()>> queued;
    auto enqueue = [&](std::function<void()> fn) -> bool {
        queued.push_back(std::move(fn));
        return true;
    };
    int applied = 0;
    auto process = oreslang::runtime::install_source_supervisor_stop(
        source, 77, 42, 2, enqueue,
        [&](const oreslang::runtime::SupervisorProcess::Resolution& decision) {
            check(decision.requested ==
                    oreslang::runtime::SupervisorProcess::Directive::stop,
                  "native source STOP policy emitted wrong directive");
            ++applied;
        });
    process->accept(oreslang::runtime::SupervisorFailureNotice{
        77, 42, 9, 1, std::nullopt, "finalized panic",
        ORES_NATIVE_CONTROL_PANIC, 37, 20261011});
    unsigned ticks = 0;
    while (!queued.empty()) {
        check(++ticks < 20, "runaway supervisor CONTROL");
        auto task = std::move(queued.front());
        queued.pop_front();
        task();
    }
    check(applied == 1 && process->resolved() == 1 &&
            process->undelivered() == 0 && process->healthy(),
          "native guest STOP failed exactly-once CONTROL commit");


    // Compiled typed Failure STOP must preserve the actor incarnation and
    // generation supplied by the trusted host notice, not synthesize or
    // accept an arbitrary target chosen by guest source.
    unsigned targeted_applied = 0;
    auto targeted_process = oreslang::runtime::install_source_supervisor_stop(
        with_failure, 97, 62, 2, enqueue,
        [&](const oreslang::runtime::SupervisorProcess::Resolution& decision) {
            check(decision.requested ==
                      oreslang::runtime::SupervisorProcess::Directive::stop,
                  "bound Failure did not lower to STOP");
            check(decision.notice.actor_id == 19
                      && decision.notice.generation == 7
                      && decision.notice.supervisor_id == 97
                      && decision.notice.runtime_id == 62,
                  "typed Failure STOP changed the authorized incarnation");
            ++targeted_applied;
        });
    targeted_process->accept(oreslang::runtime::SupervisorFailureNotice{
        97, 62, 19, 7, std::nullopt, "bound typed failure",
        ORES_NATIVE_CONTROL_PANIC, 38, 20261011});
    while (!queued.empty()) {
        check(++ticks < 60, "runaway typed Failure CONTROL");
        auto task = std::move(queued.front());
        queued.pop_front();
        task();
    }
    check(targeted_applied == 1 && targeted_process->resolved() == 1
              && targeted_process->undelivered() == 0,
          "typed Failure STOP did not commit exactly once");



    // The restricted f.child projection must come from the exact verified
    // logical-child binding in the host terminal PANIC envelope.
    unsigned child_applied = 0;
    auto child_process = oreslang::runtime::install_source_supervisor_stop(
        with_child, 107, 72, 2, enqueue,
        [&](const oreslang::runtime::SupervisorProcess::Resolution& decision) {
            check(decision.notice.actor_id == 29
                      && decision.notice.generation == 11
                      && decision.notice.logical_child_id == 7001
                      && decision.notice.logical_incarnation == 3,
                  "checked child STOP lost its exact logical/physical identity");
            ++child_applied;
        });
    child_process->accept(oreslang::runtime::SupervisorFailureNotice{
        107, 72, 29, 11, std::nullopt, "logical child failure",
        ORES_NATIVE_CONTROL_PANIC, 49, 20261011, 7001, 3});
    while (!queued.empty()) {
        check(++ticks < 70, "runaway logical child CONTROL");
        auto task = std::move(queued.front());
        queued.pop_front();
        task();
    }
    check(child_applied == 1 && child_process->resolved() == 1,
          "bound f.child STOP failed exactly once");

    auto missing_child = oreslang::runtime::install_source_supervisor_stop(
        with_child, 108, 73, 2, enqueue,
        [&](const oreslang::runtime::SupervisorProcess::Resolution&) {
            throw std::runtime_error("unbound Failure.child was applied");
        });
    missing_child->accept(oreslang::runtime::SupervisorFailureNotice{
        108, 73, 30, 12, std::nullopt, "unbound logical child",
        ORES_NATIVE_CONTROL_PANIC, 50, 20261011});
    while (!queued.empty()) {
        check(++ticks < 90, "runaway missing logical child CONTROL");
        auto task = std::move(queued.front());
        queued.pop_front();
        task();
    }
    check(missing_child->resolved() == 0 && missing_child->undelivered() == 1
              && !missing_child->healthy(),
          "f.child projection accepted missing trusted identity");

    // A legacy untyped host notice may use the historical no-argument
    // STOP compatibility form, but must not impersonate a typed Failure
    // parameter in an opt-in source hook.
    unsigned untyped_applied = 0;
    auto untyped = oreslang::runtime::install_source_supervisor_stop(
        with_failure, 98, 63, 2, enqueue,
        [&](const oreslang::runtime::SupervisorProcess::Resolution&) {
            ++untyped_applied;
        });
    untyped->accept(oreslang::runtime::SupervisorFailureNotice{
        98, 63, 20, 8, std::nullopt, "untyped legacy terminal"});
    while (!queued.empty()) {
        check(++ticks < 80, "runaway untyped Failure CONTROL");
        auto task = std::move(queued.front());
        queued.pop_front();
        task();
    }
    check(untyped_applied == 0 && untyped->resolved() == 0
              && untyped->undelivered() == 1 && !untyped->healthy(),
          "untyped native notice forged the explicit Failure binder");

    // Even zero-argument self.stop() is compiled inside a typed
    // on_panic(f: Failure). A legacy, zero-kind host notice has no validated
    // PANIC provenance and must never trigger a guest source lifecycle effect.
    unsigned legacy_applied = 0;
    auto legacy = oreslang::runtime::install_source_supervisor_stop(
        source, 99, 64, 2, enqueue,
        [&](const oreslang::runtime::SupervisorProcess::Resolution&) {
            ++legacy_applied;
        });
    legacy->accept(oreslang::runtime::SupervisorFailureNotice{
        99, 64, 21, 9, std::nullopt, "legacy untyped zero-arg terminal"});
    while (!queued.empty()) {
        check(++ticks < 100, "runaway legacy CONTROL");
        auto task = std::move(queued.front());
        queued.pop_front();
        task();
    }
    check(legacy_applied == 0 && legacy->resolved() == 0
              && legacy->undelivered() == 1 && !legacy->healthy(),
          "zero-argument source STOP accepted untyped legacy notice");

    // Cancellation is *not* silently treated as a panic STOP decision.
    auto cancelled = oreslang::runtime::install_source_supervisor_stop(
        source, 88, 43, 2, enqueue,
        [&](const oreslang::runtime::SupervisorProcess::Resolution&) {
            throw std::runtime_error("cancelled child applied STOP");
        });
    cancelled->accept(oreslang::runtime::SupervisorFailureNotice{
        88, 43, 10, 1, std::nullopt, "cancelled",
        ORES_NATIVE_CONTROL_CANCELLED, 0, 0});
    while (!queued.empty()) {
        check(++ticks < 40, "runaway cancelled CONTROL");
        auto task = std::move(queued.front());
        queued.pop_front();
        task();
    }
    check(!cancelled->healthy() && cancelled->resolved() == 0 &&
          cancelled->undelivered() == 1 && !cancelled->callback_active(),
          "native STOP-only hook accepted cancellation");

    // Trusted registry-bound STOP: an identity in the envelope is not
    // authority without a live, exactly-finalized registry snapshot.
    for (int case_id = 0; case_id < 6; ++case_id) {
        const auto supervisor = static_cast<std::uint64_t>(700 + case_id);
        const auto domain = static_cast<std::uint64_t>(900 + case_id);
        auto children = std::make_shared<oreslang::runtime::SupervisorChildRegistry>(
            supervisor, domain);
        const OresNativeActorRef actor{domain, 51, 1};
        const auto admitted_child = children->register_child(actor);
        if (case_id != 1) {
            children->finalized(admitted_child.child, actor, 1);
        }
        if (case_id == 2) {
            children->stop(admitted_child.child, actor, 1);
        }
        oreslang::runtime::SupervisorFailureNotice failure{
            supervisor, domain, actor.actor_slot, actor.generation,
            std::nullopt, "typed terminal panic",
            ORES_NATIVE_CONTROL_PANIC, 23, 4242,
            admitted_child.child.logical_child_id, admitted_child.incarnation};
        if (case_id == 3) ++failure.actor_id;
        if (case_id == 4) ++failure.logical_incarnation;
        if (case_id == 5) failure.logical_child_id = 0;

        std::deque<std::function<void()>> locally_queued;
        auto enqueue_verified = [&](std::function<void()> cb) -> bool {
            locally_queued.push_back(std::move(cb));
            return true;
        };
        int sink_calls = 0;
        auto verified = oreslang::runtime::install_source_supervisor_stop_verified(
            source, supervisor, domain, 2, enqueue_verified,
            children,
            [&](const oreslang::runtime::SupervisorProcess::Resolution&) {
                ++sink_calls;
            });
        verified->accept(failure);
        unsigned loop = 0;
        while (!locally_queued.empty()) {
            check(++loop < 32, "verified native STOP work queue never drained");
            auto run = std::move(locally_queued.front());
            locally_queued.pop_front();
            run();
        }
        if (case_id == 0) {
            check(sink_calls == 1 && verified->resolved() == 1
                    && verified->undelivered() == 0 && verified->healthy(),
                  "verified native STOP did not acknowledge finalized target");
            check(children->snapshot(admitted_child.child).phase
                    == oreslang::runtime::SupervisorChildRegistry::Phase::stopped,
                  "verified STOP effect did not retire logical incarnation");
        } else {
            check(sink_calls == 0 && verified->resolved() == 0
                    && verified->undelivered() == 1 && !verified->healthy(),
                  "unfinalized/stale native Failure reached STOP effect");
            check(children->snapshot(admitted_child.child).phase
                    != oreslang::runtime::SupervisorChildRegistry::Phase::stopped
                    || case_id == 2,
                  "rejected STOP changed registered child lifecycle");
        }
        verified->close();
    }

    std::cout << "NATIVE_SOURCE_SUPERVISOR_STOP_SLICE_PASS parsed=1 "
                 "typed=1 stop=1 cancelled=0 default_admission=closed\n";
}
