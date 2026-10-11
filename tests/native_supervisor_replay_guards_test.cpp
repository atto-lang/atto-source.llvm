#include "oreslang/runtime/SupervisorFailureControl.hpp"
#include "oreslang/runtime/SupervisorProcess.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

using oreslang::runtime::SupervisorFailureControl;
using oreslang::runtime::SupervisorFailureNotice;
using oreslang::runtime::SupervisorProcess;

static void check(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

// Intentionally nonconforming adapter: every accepted callback is replayed
// once, including after the next logical turn is already scheduled.
struct ReplayingEnqueue {
    std::deque<std::function<void()>> callbacks;
    bool enqueue(std::function<void()> callback) {
        callbacks.push_back(callback);
        callbacks.push_back(std::move(callback));
        return true;
    }
    void step() {
        if (callbacks.empty()) throw std::runtime_error("no callback to execute");
        auto callback = std::move(callbacks.front());
        callbacks.pop_front();
        callback();
    }
    void drive() {
        std::size_t steps = 0;
        while (!callbacks.empty()) {
            check(++steps < 1000, "unbounded callback replay");
            step();
        }
    }
};

int main() {
    // Duplication across a 32-notice drain batch cannot invoke the handler
    // again or let the old runnable steal the following batch.
    {
        auto scheduler = std::make_shared<ReplayingEnqueue>();
        auto lane = std::make_shared<SupervisorFailureControl>(101, 40);
        int processed = 0;
        lane->install([&](const SupervisorFailureNotice&) { ++processed; },
            [scheduler](std::function<void()> work) {
                return scheduler->enqueue(std::move(work));
            });
        for (std::uint64_t n = 1; n <= 34; ++n)
            lane->publish(SupervisorFailureNotice{101, 2, n, 1, std::nullopt, "panic"});
        scheduler->step(); // First scheduled drain handles exactly 32.
        check(processed == 32 && lane->pending() == 2,
              "CONTROL batch did not process precisely 32 notices");
        scheduler->step(); // Stale duplicate of the first generation.
        check(processed == 32 && lane->pending() == 2,
              "stale CONTROL callback consumed a subsequent batch");
        scheduler->drive();
        check(processed == 34 && lane->pending() == 0 && lane->healthy(),
              "duplicated CONTROL callbacks lost or replayed notices");
    }

    // Duplicate begin and duplicate resume tasks must never run policy twice
    // or misapply an earlier directive to a newly queued failure.
    {
        auto scheduler = std::make_shared<ReplayingEnqueue>();
        int policies = 0;
        int effects = 0;
        auto supervisor = std::make_shared<SupervisorProcess>(
            201, 300, 4,
            [scheduler](std::function<void()> work) {
                return scheduler->enqueue(std::move(work));
            },
            [&](const SupervisorFailureNotice&)
                -> std::optional<SupervisorProcess::Directive> {
                ++policies;
                return std::nullopt; // Retain one logical turn across await.
            },
            [&](const SupervisorProcess::Resolution&) { ++effects; });
        supervisor->accept(SupervisorFailureNotice{201, 300, 1, 1, std::nullopt, "first"});
        supervisor->accept(SupervisorFailureNotice{201, 300, 2, 1, std::nullopt, "second"});
        scheduler->drive(); // Replayed begin(1) cannot run policy twice.
        check(policies == 1 && supervisor->callback_active(),
              "duplicate begin invoked two supervisor policies");

        auto first = supervisor->current_epoch();
        check(first != 0 && supervisor->resume(first, SupervisorProcess::Directive::stop),
              "failed to resume the first logical turn");
        scheduler->step(); // First finish commits turn one, schedules begin(2).
        check(effects == 1 && supervisor->resolved() == 1,
              "first resume did not commit exactly once");
        scheduler->step(); // Duplicate stale finish(1).
        check(effects == 1 && supervisor->resolved() == 1,
              "stale resume committed a second queued notice");
        scheduler->drive(); // Legitimate begin(2), duplicate suppressed.
        check(policies == 2 && effects == 1 && supervisor->callback_active(),
              "next supervisor turn lost its exclusive policy lease");

        auto second = supervisor->current_epoch();
        check(second != first && !supervisor->resume(first, SupervisorProcess::Directive::stop),
              "previous generation resumed the current supervisor turn");
        check(supervisor->resume(second, SupervisorProcess::Directive::stop),
              "current generation could not resume");
        scheduler->drive();
        check(policies == 2 && effects == 2 && supervisor->resolved() == 2
              && supervisor->pending() == 0 && supervisor->healthy(),
              "duplicated resume corrupted supervisor lifecycle accounting");
    }

    std::cout << "NATIVE_SUPERVISOR_REPLAY_PASS drain=34 begin=2 resume=2\n";
}
