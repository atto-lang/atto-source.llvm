#include "oreslang/runtime/ActorGroupLifecycle.hpp"
#include "oreslang/runtime/SupervisorControlScheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::SupervisorProcess;
using oreslang::runtime::SupervisorControlScheduler;
using namespace std::chrono_literals;

static void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

struct SaturatedActorCarriers {
    std::mutex mutex;
    std::condition_variable cv;
    int occupied = 0;
    bool release = false;
    std::vector<std::thread> threads;

    explicit SaturatedActorCarriers(int count) {
        for (int i = 0; i < count; ++i) {
            threads.emplace_back([this] {
                std::unique_lock<std::mutex> lock(mutex);
                ++occupied;
                cv.notify_all();
                cv.wait(lock, [this] { return release; });
            });
        }
    }
    bool saturated() {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 5s, [this] {
            return occupied == static_cast<int>(threads.size());
        });
    }
    ~SaturatedActorCarriers() {
        {
            std::lock_guard<std::mutex> guard(mutex);
            release = true;
        }
        cv.notify_all();
        for (auto& t : threads) if (t.joinable()) t.join();
    }
};

int main() {
    constexpr int families = 192;
    auto control = std::make_shared<SupervisorControlScheduler>(2, 1024);
    auto enqueue = control->adapter();

    SaturatedActorCarriers busyActors(2);
    check(busyActors.saturated(), "actor carrier saturation not established");

    std::mutex resultsMutex;
    std::condition_variable resultsCv;
    int delivered = 0;
    std::atomic<int> wrongDelivery{0};
    std::vector<std::shared_ptr<SupervisorProcess>> processes;
    std::vector<std::unique_ptr<NativeActorGroup>> groups;
    processes.reserve(families);
    groups.reserve(families);

    for (int i = 1; i <= families; ++i) {
        const auto runtimeId = static_cast<std::uint64_t>(9000 + i);
        auto group = std::make_unique<NativeActorGroup>(runtimeId, 2);
        auto policy = [runtimeId](const auto& event)
                -> std::optional<SupervisorProcess::Directive> {
            if (event.runtime_id != runtimeId) throw std::runtime_error("foreign notice");
            return SupervisorProcess::Directive::escalate;
        };
        auto process = group->install_supervisor_process(
            2, enqueue, policy,
            [&](const SupervisorProcess::Resolution& result) {
                if (result.requested != SupervisorProcess::Directive::escalate
                    || result.notice.diagnostic != "terminal")
                    wrongDelivery.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lock(resultsMutex);
                    ++delivered;
                }
                resultsCv.notify_one();
            });
        auto actor = group->adopt(runtimeId, 1, 1, [] {});
        check(group->mark_ready(actor), "host actor readiness failed");
        check(group->finalized(actor, "terminal"),
              "host actor could not finalize and publish CONTROL");
        processes.push_back(std::move(process));
        groups.push_back(std::move(group));
    }

    {
        std::unique_lock<std::mutex> lock(resultsMutex);
        check(resultsCv.wait_for(lock, 10s, [&] { return delivered == families; }),
              "supervisor CONTROL starved behind saturated user actor carriers");
    }
    check(wrongDelivery.load() == 0, "misrouted supervisor CONTROL notice");
    for (const auto& process : processes) {
        check(process->resolved() == 1 && process->healthy()
              && process->undelivered() == 0,
              "per-supervisor admission or resolution mismatch");
    }

    // Close-versus-resume: a detached async completion cannot restart an
    // actor after its supervisor process was explicitly closed.
    auto suspended = std::make_shared<SupervisorProcess>(
        711, 711, 2, enqueue,
        [](const auto&) -> std::optional<SupervisorProcess::Directive> {
            return std::nullopt;
        },
        [&](const auto&) { wrongDelivery.fetch_add(1); });
    suspended->accept(oreslang::runtime::SupervisorFailureNotice{
        711, 711, 1, 1, std::nullopt, "terminal"});
    // Accept is asynchronous; wait until callback owns its logical lane.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (suspended->current_epoch() == 0
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto oldEpoch = suspended->current_epoch();
    check(oldEpoch != 0, "suspended CONTROL callback never admitted");
    suspended->close();
    check(!suspended->resume(oldEpoch, SupervisorProcess::Directive::restart),
          "closed supervisor admitted late restart");
    check(suspended->resolved() == 0 && suspended->undelivered() == 1,
          "close-versus-resume misreported an unresolved decision");

    // Closing concurrently from two host threads must never race on joinable
    // std::thread handles. Both close requests must be idempotent.
    std::thread close_a([control] { control->close(); });
    std::thread close_b([control] { control->close(); });
    close_a.join();
    close_b.join();
    check(!enqueue([] {}), "closed CONTROL scheduler admitted new work");
    auto rejected = std::make_shared<SupervisorProcess>(
        712, 712, 2, enqueue,
        [](const auto&) -> std::optional<SupervisorProcess::Directive> {
            return SupervisorProcess::Directive::restart;
        },
        [&](const auto&) { wrongDelivery.fetch_add(1); });
    bool admission_rejected = false;
    try {
        rejected->accept(oreslang::runtime::SupervisorFailureNotice{
            712, 712, 1, 1, std::nullopt, "terminal"});
    } catch (const std::logic_error&) {
        admission_rejected = true;
    }
    check(admission_rejected, "closed CONTROL dispatcher did not report failed admission");
    check(!rejected->healthy() && rejected->undelivered() == 1
          && rejected->resolved() == 0,
          "closed scheduler silently accepted CONTROL");
    check(wrongDelivery.load() == 0, "CONTROL executed invalid decision");
}
