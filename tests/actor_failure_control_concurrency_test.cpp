#include "oreslang/runtime/ActorFailureControl.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

using namespace oreslang::runtime;

static int check_concurrent_fifo() {
    ActorFailureControlInbox inbox("parent", 16);
    constexpr int kMessages = 2048;
    constexpr int kWindow = 8; // Always below ring capacity: no expected drops.
    std::mutex progress_mutex;
    std::condition_variable progress_cv;
    int sent = 0;
    int received = 0;
    bool failed = false;

    const auto fail = [&] {
        {
            std::lock_guard<std::mutex> lock(progress_mutex);
            failed = true;
        }
        progress_cv.notify_all();
    };

    // Exercise producer/consumer overlap with bounded credit, not 200,000
    // yield retries whose duration depends on the CI runner's scheduling.
    // A rejected offer is a real dropped notice and must fail this fixture.
    std::thread producer([&] {
        for (int i = 0; i < kMessages; ++i) {
            {
                std::unique_lock<std::mutex> lock(progress_mutex);
                if (!progress_cv.wait_for(lock, std::chrono::seconds(10),
                        [&] { return failed || sent - received < kWindow; })) {
                    lock.unlock();
                    fail();
                    return;
                }
                if (failed) return;
            }
            auto notice = ActorFailureControlNotice::from_guest_effect(
                    ActorExceptionalEffect::Raise,
                    "actor-" + std::to_string(i), "parent");
            if (!inbox.offer(notice, notice.envelope().actor_id())) {
                fail();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(progress_mutex);
                sent = i + 1;
            }
            progress_cv.notify_all();
        }
    });

    std::thread consumer([&] {
        for (int i = 0; i < kMessages; ++i) {
            {
                std::unique_lock<std::mutex> lock(progress_mutex);
                if (!progress_cv.wait_for(lock, std::chrono::seconds(10),
                        [&] { return failed || sent > i; })) {
                    lock.unlock();
                    fail();
                    return;
                }
                if (failed) return;
            }
            const auto next = inbox.try_pop();
            if (!next || next->envelope().actor_id()
                    != "actor-" + std::to_string(i)
                    || next->effect() != ActorExceptionalEffect::Raise) {
                fail();
                return;
            }
            {
                std::lock_guard<std::mutex> lock(progress_mutex);
                received = i + 1;
            }
            progress_cv.notify_all();
        }
    });

    producer.join();
    consumer.join();
    if (failed || inbox.dropped() != 0 || sent != kMessages
            || received != kMessages) return 1;
    if (inbox.pending() != 0 || inbox.retired()) return 2;
    return 0;
}

static int check_retirement_race() {
    ActorFailureControlInbox inbox("parent", 16);
    constexpr int kAttempts = 512;
    std::atomic<int> attempted{0};
    std::thread producer([&] {
        const auto notice = ActorFailureControlNotice::from_guest_effect(
                ActorExceptionalEffect::Panic, "child", "parent");
        for (int i = 0; i < kAttempts; ++i) {
            (void)inbox.offer(notice, notice.envelope().actor_id());
            attempted.fetch_add(1);
        }
    });
    while (attempted.load() < 32) std::this_thread::yield();
    inbox.retire();
    producer.join();

    // No consumer exists: every attempted notice was either rejected at
    // admission or accepted and then accounted for at retirement.
    if (!inbox.retired() || inbox.pending() != 0) return 3;
    if (inbox.dropped() != static_cast<std::size_t>(kAttempts)) return 4;
    const auto notice = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "later-child", "parent");
    if (inbox.offer(notice, notice.envelope().actor_id())) return 5;
    if (inbox.dropped() != static_cast<std::size_t>(kAttempts + 1)) return 6;
    return 0;
}

int main() {
    if (int status = check_concurrent_fifo()) return status;
    return check_retirement_race();
}
