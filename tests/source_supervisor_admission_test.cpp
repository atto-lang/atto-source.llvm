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
    reject_supervisor(with_failure); // general native compiler remains closed
    for (const auto forged : {
            "self.stop(other); return;",
            "self.stop(self); return;",
            "self.stop(problem.child); return;",
            "self.stop(problem.actor); return;",
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

    std::cout << "NATIVE_SOURCE_SUPERVISOR_STOP_SLICE_PASS parsed=1 "
                 "typed=1 stop=1 cancelled=0 default_admission=closed\n";
}
