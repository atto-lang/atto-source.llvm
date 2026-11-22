#include "oreslang/runtime/NativeJoinTaskWaitV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

using oreslang::runtime::NativeJoinTaskWaitV1;
using oreslang::runtime::NativeJoinCompletionV1;
using oreslang::runtime::NativeThreadJoinReaperV1;

// Host-only tests. All polling below belongs to the test harness, never to
// the AttoLang scheduler/guest implementation.
struct Argument {
    int value;
    std::atomic<bool>* gate;
};
static int entry(void* raw) {
    const auto* a = static_cast<Argument*>(raw);
    if (a->gate) {
        while (!a->gate->load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    return a->value;
}
static void destroy(void* raw) { delete static_cast<Argument*>(raw); }

static std::shared_ptr<NativeJoinCompletionV1> start(
        NativeThreadJoinReaperV1& reaper, std::atomic<bool>* gate = nullptr) {
    auto* arg = new Argument{17, gate};
    ores_native_thread_v1* thread = nullptr;
    if (ores_native_thread_spawn_v1(entry, arg, destroy, &thread) !=
        ORES_THREAD_OK_V1) {
        delete arg; // failed spawn does not transfer ownership
        return {};
    }
    auto completion = reaper.submit(thread);
    if (!completion) { // failed submission preserves the native join right
        int result = 0;
        assert(ores_native_thread_join_blocking_v1(thread, &result) == ORES_THREAD_OK_V1);
        assert(ores_native_thread_release_v1(thread) == ORES_THREAD_OK_V1);
    }
    return completion;
}
template <class Predicate>
static void within(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "native join task waiter timeout\n";
            assert(false);
        }
        std::this_thread::yield();
    }
}
static void joined(const std::shared_ptr<NativeJoinCompletionV1>& completion) {
    assert(completion);
    within([&] {
        ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
        int value = -1;
        if (!completion->try_read(status, value)) return false;
        assert(status == ORES_THREAD_OK_V1 && value == 17);
        return true;
    });
}
static void test_invalid_and_competing_registration(NativeThreadJoinReaperV1& reaper) {
    using State = NativeJoinTaskWaitV1::State;
    assert(!NativeJoinTaskWaitV1::create(0, [](std::uint64_t) {}));
    assert(!NativeJoinTaskWaitV1::create(1, {}));
    auto waiter = NativeJoinTaskWaitV1::create(1, [](std::uint64_t) {});
    assert(waiter && !waiter->attach({}));
    std::atomic<bool> gate{false};
    auto completed = start(reaper, &gate);
    assert(completed && waiter->attach(completed));
    assert(!waiter->attach(completed));
    std::atomic<int> forbidden{0};
    auto loser = NativeJoinTaskWaitV1::create(2, [&](std::uint64_t) {
        forbidden.fetch_add(1);
    });
    assert(loser && !loser->attach(completed));
    assert(loser->state() == State::Cancelled);
    gate.store(true, std::memory_order_release);
    joined(completed);
    within([&] { return waiter->state() != State::Pending; });
    assert(forbidden.load() == 0);
}
static void test_cancel_completion_races(NativeThreadJoinReaperV1& reaper) {
    using State = NativeJoinTaskWaitV1::State;
    for (int i = 0; i < 200; ++i) {
        std::atomic<bool> gate{false};
        auto completed = start(reaper, &gate);
        assert(completed);
        auto hits = std::make_shared<std::atomic<int>>(0);
        auto waiter = NativeJoinTaskWaitV1::create(
            static_cast<std::uint64_t>(i + 1), [hits](std::uint64_t generation) {
                assert(generation != 0);
                // The scheduler must enqueue, not directly execute guest code.
                assert(hits->fetch_add(1, std::memory_order_acq_rel) == 0);
            });
        assert(waiter && waiter->attach(completed));
        assert(!waiter->attach(completed));
        if (i % 4 == 0) {
            assert(waiter->cancel()); // cancel definitely wins
            gate.store(true, std::memory_order_release);
        } else if (i % 4 == 1) {
            gate.store(true, std::memory_order_release); // completion can win
        } else {
            // Genuine cancellation-vs-completion racing paths.
            std::thread cancel_thread([waiter] { (void)waiter->cancel(); });
            gate.store(true, std::memory_order_release);
            cancel_thread.join();
        }
        joined(completed);
        within([&] { return waiter->state() != State::Pending; });
        if (waiter->state() == State::Notified) {
            within([&] { return hits->load(std::memory_order_acquire) == 1; });
        } else {
            assert(waiter->state() == State::Cancelled);
            assert(hits->load(std::memory_order_acquire) == 0);
        }
    }
}
static void test_after_completion_and_weak_retirement(
        NativeThreadJoinReaperV1& reaper) {
    using State = NativeJoinTaskWaitV1::State;
    auto already_done = start(reaper);
    joined(already_done);
    std::atomic<int> late{0};
    auto waiter = NativeJoinTaskWaitV1::create(123, [&](std::uint64_t g) {
        assert(g == 123);
        late.fetch_add(1);
    });
    assert(waiter && waiter->attach(already_done)); // notify inline
    assert(waiter->state() == State::Notified);
    assert(late.load() == 1);
    assert(!waiter->cancel());
    assert(!waiter->attach(already_done));

    std::atomic<bool> gate{false};
    auto completed = start(reaper, &gate);
    std::atomic<int> resurrected{0};
    auto dropped = NativeJoinTaskWaitV1::create(124, [&](std::uint64_t) {
        resurrected.fetch_add(1);
    });
    std::weak_ptr<NativeJoinTaskWaitV1> weak = dropped;
    assert(dropped->attach(completed));
    dropped.reset(); // subscription must not retain logical execution
    assert(weak.expired());
    gate.store(true, std::memory_order_release);
    joined(completed);
    assert(resurrected.load() == 0);

    std::atomic<bool> gate2{false};
    auto cancelled = start(reaper, &gate2);
    auto discarded = NativeJoinTaskWaitV1::create(125, [&](std::uint64_t) {
        resurrected.fetch_add(1000);
    });
    assert(discarded->cancel());
    assert(discarded->attach(cancelled));
    gate2.store(true, std::memory_order_release);
    joined(cancelled);
    assert(discarded->state() == State::Cancelled);
    assert(resurrected.load() == 0);
}
static void test_stale_generation_admission(NativeThreadJoinReaperV1& reaper) {
    std::atomic<std::uint64_t> active_generation{100};
    std::atomic<int> accepted{0};
    std::atomic<bool> gate{false};
    auto completed = start(reaper, &gate);
    // This is a mock of the scheduler's required generation check.
    auto waiter = NativeJoinTaskWaitV1::create(100, [&](std::uint64_t gen) {
        if (active_generation.load(std::memory_order_acquire) == gen)
            accepted.fetch_add(1);
    });
    assert(waiter && waiter->attach(completed));
    active_generation.store(101, std::memory_order_release);
    gate.store(true, std::memory_order_release);
    joined(completed);
    within([&] { return waiter->state() != NativeJoinTaskWaitV1::State::Pending; });
    assert(accepted.load() == 0);
}
int main() {
    NativeThreadJoinReaperV1 reaper(8, 2);
    test_invalid_and_competing_registration(reaper);
    test_cancel_completion_races(reaper);
    test_after_completion_and_weak_retirement(reaper);
    test_stale_generation_admission(reaper);
    std::cout << "native join task wait notification: OK (host-only)\n";
}
