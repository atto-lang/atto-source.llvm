#include "oreslang/runtime/ActorFailureControl.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
            // Registration models the trusted native actor spawn path.
            // Revoke only after admission, never by echoing a notice ID as
            // sufficient authorization for the queue.
            const auto& child_id = notice.envelope().actor_id();
            const auto permit = inbox.register_direct_child(child_id);
            if (!permit
                    || !inbox.offer(notice, *permit)
                    || !inbox.unregister_direct_child(*permit)) {
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
    std::vector<ActorFailureControlInbox::DirectChildPermit> permits;
    permits.reserve(kAttempts);
    for (int i = 0; i < kAttempts; ++i) {
        const auto permit = inbox.register_direct_child("child-" + std::to_string(i));
        if (!permit) return 13;
        permits.push_back(*permit);
    }
    const auto later = inbox.register_direct_child("later-child");
    if (!later) return 21;
    std::atomic<int> attempted{0};
    std::thread producer([&] {
        for (int i = 0; i < kAttempts; ++i) {
            const auto child = "child-" + std::to_string(i);
            const auto notice = ActorFailureControlNotice::from_guest_effect(
                    ActorExceptionalEffect::Panic, child, "parent");
            // All authority was issued before the retirement race.
            (void)inbox.offer(notice, permits[static_cast<std::size_t>(i)]);
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
    if (inbox.offer(notice, *later)) return 5;
    if (inbox.dropped() != static_cast<std::size_t>(kAttempts + 1)) return 6;
    return 0;
}

// Retiring a supervisor must permanently close its CONTROL admission. A
// different supervisor's inbox must remain independent under the same event.
static int check_supervisor_retirement_isolation() {
    ActorFailureControlInbox left("left", 1);
    ActorFailureControlInbox right("right", 1);
    const auto left_notice = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "left-child", "left");
    const auto right_notice = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Panic, "right-child", "right");
    const auto leftPermit = left.register_direct_child("left-child");
    const auto rightPermit = right.register_direct_child("right-child");
    if (!leftPermit || !rightPermit) return 14;
    if (!left.offer(left_notice, *leftPermit)) return 7;
    if (!right.offer(right_notice, *rightPermit)) return 8;
    left.retire();
    left.retire();
    if (left.pending() != 0 || left.dropped() != 1 || left.try_pop()) return 9;
    if (left.offer(left_notice, *leftPermit) || left.dropped() != 2) return 10;
    if (right.pending() != 1 || right.dropped() != 0 || right.retired()) return 11;
    const auto received = right.try_pop();
    if (!received || received->effect() != ActorExceptionalEffect::Panic
            || received->envelope().actor_id() != "right-child") return 12;
    return 0;
}

// Both a reader and the retirement path remove notices, but never twice.
// Every attempted admission must be either consumed or counted as dropped.
static int check_consumer_retirement_accounting() {
    ActorFailureControlInbox inbox("parent", 8);
    constexpr int kAttempts = 2048;
    std::vector<ActorFailureControlInbox::DirectChildPermit> permits;
    permits.reserve(kAttempts);
    for (int i = 0; i < kAttempts; ++i) {
        const auto permit = inbox.register_direct_child("child-" + std::to_string(i));
        if (!permit) return 22;
        permits.push_back(*permit);
    }
    std::atomic<int> attempted{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> unexpected{false};

    std::thread producer([&] {
        for (int i = 0; i < kAttempts; ++i) {
            auto notice = ActorFailureControlNotice::from_guest_effect(
                    ActorExceptionalEffect::Raise,
                    "child-" + std::to_string(i), "parent");
            // The issuer owns all permits before retirement can begin.
            // Terminal offers made after retirement must count as dropped.
            (void)inbox.offer(notice, permits[static_cast<std::size_t>(i)]);
            attempted.fetch_add(1);
            if ((i & 15) == 0) std::this_thread::yield();
        }
        producer_done.store(true);
    });
    std::thread consumer([&] {
        int previous = -1;
        while (!producer_done.load() || !inbox.retired()) {
            auto notice = inbox.try_pop();
            if (!notice) {
                std::this_thread::yield();
                continue;
            }
            const auto& id = notice->envelope().actor_id();
            if (id.rfind("child-", 0) != 0) {
                unexpected.store(true);
                continue;
            }
            const int sequence = std::stoi(id.substr(6));
            if (sequence <= previous ||
                    notice->effect() != ActorExceptionalEffect::Raise) {
                unexpected.store(true);
            }
            previous = sequence;
            consumed.fetch_add(1);
        }
    });
    while (attempted.load() < 32) std::this_thread::yield();
    inbox.retire();
    producer.join();
    consumer.join();
    if (unexpected.load() || !inbox.retired() || inbox.pending() != 0) return 13;
    if (static_cast<std::size_t>(consumed.load()) + inbox.dropped()
            != static_cast<std::size_t>(kAttempts)) return 14;
    if (inbox.try_pop().has_value()) return 15;
    return 0;
}

// Concurrent, same-generation terminal publications must linearize to one
// accepted event. Rejected replays are counted independently of overflow.
static int check_duplicate_terminal_race() {
    ActorFailureControlInbox inbox("parent", 16);
    const auto permit = inbox.register_direct_child("child");
    if (!permit) return 16;
    constexpr int kWriters = 16;
    std::atomic<bool> start{false};
    std::atomic<int> accepted{0};
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto notice = ActorFailureControlNotice::from_guest_effect(
                    i % 2 == 0 ? ActorExceptionalEffect::Raise
                               : ActorExceptionalEffect::Panic,
                    "child", "parent");
            if (inbox.offer(notice, *permit)) ++accepted;
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& writer : writers) writer.join();
    if (accepted.load() != 1 || inbox.pending() != 1
            || inbox.duplicate_rejections() != kWriters - 1
            || inbox.dropped() != 0) return 17;
    if (!inbox.try_pop() || inbox.try_pop()) return 18;
    const auto replay = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Panic, "child", "parent");
    if (inbox.offer(replay, *permit) || inbox.duplicate_rejections() != kWriters) return 19;
    if (!inbox.unregister_direct_child(*permit)) return 20;
    return 0;
}

// ActorIds are not sufficient proof of generation. Many obsolete callbacks
// may race with a replacement registration bearing the same opaque string.
// The original permit must never publish or revoke the new incarnation.
static int check_reused_id_stale_permit_race() {
    ActorFailureControlInbox inbox("parent", 8);
    const auto stale = inbox.register_direct_child("reused");
    if (!stale || !inbox.unregister_direct_child(*stale)) return 30;
    const auto current = inbox.register_direct_child("reused");
    if (!current) return 31;

    constexpr int kWriters = 8;
    constexpr int kAttempts = 128;
    std::atomic<bool> start{false};
    std::atomic<int> denied{0};
    std::atomic<int> violation{0};
    std::vector<std::thread> writers;
    for (int worker = 0; worker < kWriters; ++worker) {
        writers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kAttempts; ++i) {
                const auto notice = ActorFailureControlNotice::from_guest_effect(
                        ActorExceptionalEffect::Panic, "reused", "parent");
                try {
                    // Returning true OR false without rejecting stale
                    // authority is a bug; rejected old permits must throw.
                    (void)inbox.offer(notice, *stale);
                    violation.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::invalid_argument&) {
                    denied.fetch_add(1, std::memory_order_relaxed);
                }
                if (inbox.unregister_direct_child(*stale)) {
                    violation.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : writers) worker.join();

    if (violation.load() != 0 || denied.load() != kWriters * kAttempts
            || inbox.pending() != 0 || inbox.dropped() != 0) return 32;
    const auto currentNotice = ActorFailureControlNotice::from_guest_effect(
            ActorExceptionalEffect::Raise, "reused", "parent");
    if (!inbox.offer(currentNotice, *current)) return 33;
    const auto admitted = inbox.try_pop();
    if (!admitted || admitted->effect() != ActorExceptionalEffect::Raise) return 34;
    if (!inbox.unregister_direct_child(*current)) return 35;
    return 0;
}

int main() {
    if (int status = check_concurrent_fifo()) return status;
    if (int status = check_retirement_race()) return status;
    if (int status = check_supervisor_retirement_isolation()) return status;
    if (int status = check_consumer_retirement_accounting()) return status;
    if (int status = check_duplicate_terminal_race()) return status;
    return check_reused_id_stale_permit_race();
}
