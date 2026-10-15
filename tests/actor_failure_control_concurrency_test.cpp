#include "oreslang/runtime/ActorFailureControl.hpp"

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>

using namespace oreslang::runtime;

static int check_concurrent_fifo() {
    ActorFailureControlInbox inbox("parent", 16);
    constexpr int kMessages = 2048;
    std::atomic<bool> producer_failed{false};
    std::atomic<bool> consumer_failed{false};

    std::thread producer([&] {
        for (int i = 0; i < kMessages; ++i) {
            auto notice = ActorFailureControlNotice::from_guest_effect(
                    ActorExceptionalEffect::Raise,
                    "actor-" + std::to_string(i), "parent");
            bool accepted = false;
            for (int retry = 0; retry < 200000; ++retry) {
                if (inbox.offer(notice, notice.envelope().actor_id())) {
                    accepted = true;
                    break;
                }
                std::this_thread::yield();
            }
            if (!accepted) {
                producer_failed.store(true);
                return;
            }
        }
    });

    std::thread consumer([&] {
        for (int i = 0; i < kMessages; ++i) {
            bool received = false;
            for (int retry = 0; retry < 200000; ++retry) {
                auto next = inbox.try_pop();
                if (next) {
                    received = next->envelope().actor_id()
                            == "actor-" + std::to_string(i)
                            && next->effect() == ActorExceptionalEffect::Raise;
                    break;
                }
                std::this_thread::yield();
            }
            if (!received) {
                consumer_failed.store(true);
                return;
            }
        }
    });
    producer.join();
    consumer.join();
    if (producer_failed || consumer_failed) return 1;
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
