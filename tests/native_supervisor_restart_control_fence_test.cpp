#include "oreslang/runtime/SupervisorProcess.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using oreslang::runtime::SupervisorProcess;
using oreslang::runtime::SupervisorFailureNotice;
using namespace std::chrono_literals;

static void check(bool yes, const char* why) {
    if (!yes) throw std::runtime_error(why);
}

int main() {
    auto make_process = [] {
        return std::make_shared<SupervisorProcess>(
            811, 912, 8,
            [](std::function<void()>) { return true; },
            [](const SupervisorFailureNotice&)
                -> std::optional<SupervisorProcess::Directive> {
                return std::nullopt;
            },
            [](const SupervisorProcess::Resolution&) {});
    };

    {
        auto process = make_process();
        std::mutex mutex;
        std::condition_variable wake;
        bool entered = false;
        bool release = false;
        std::atomic<bool> close_started{false};
        std::atomic<bool> close_done{false};
        int published = 0;
        // This test-only callback deliberately blocks to force close() to
        // contend. Runtime publication callbacks must remain bounded.
        std::thread publisher([&] {
            const auto value = process->with_healthy_restart_transaction([&] {
                std::unique_lock<std::mutex> guard(mutex);
                entered = true;
                wake.notify_all();
                wake.wait(guard, [&] { return release; });
                ++published;
                return 42;
            });
            if (value != 42) std::terminate();
        });
        {
            std::unique_lock<std::mutex> guard(mutex);
            check(wake.wait_for(guard, 3s, [&] { return entered; }),
                  "publisher failed to enter guarded CONTROL transaction");
        }
        std::thread closer([&] {
            close_started.store(true);
            process->close();
            close_done.store(true);
        });
        while (!close_started.load()) std::this_thread::yield();
        std::this_thread::sleep_for(20ms);
        const bool close_waited = !close_done.load();
        {
            std::lock_guard<std::mutex> guard(mutex);
            release = true;
        }
        wake.notify_all();
        publisher.join();
        closer.join();
        check(close_waited && close_done && published == 1
                  && !process->healthy(),
              "supervisor closed during authoritative publication");
        bool rejected = false;
        try {
            (void) process->with_healthy_restart_transaction([] { return 3; });
        } catch (const std::logic_error&) { rejected = true; }
        check(rejected, "closed supervisor admitted a future RESTART commit");
    }

    {
        auto process = make_process();
        process->close();
        int effects = 0;
        bool rejected = false;
        try {
            (void) process->with_healthy_restart_transaction([&] {
                ++effects;
                return 1;
            });
        } catch (const std::logic_error&) { rejected = true; }
        check(rejected && effects == 0,
              "closed CONTROL invoked supposedly guarded publication");
    }

    std::puts("NATIVE_SUPERVISOR_RESTART_CONTROL_FENCE_PASS close=1 stale=1");
}
