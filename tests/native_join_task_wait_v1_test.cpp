#include "oreslang/runtime/NativeJoinTaskWaitV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using oreslang::runtime::NativeJoinTaskWaitV1;
using oreslang::runtime::NativeJoinCompletionV1;

// Host-only lifecycle state test: does NOT execute compiled .atto guest code.
// A deterministic mock completion would require private reaper friendship;
// use real native pthreads and the existing reaper instead.
static int entry(void* arg) {
    return *static_cast<int*>(arg);
}
static void destroy(void* arg) { delete static_cast<int*>(arg); }
static std::shared_ptr<NativeJoinCompletionV1> start(
        oreslang::runtime::NativeThreadJoinReaperV1& reaper) {
    ores_native_thread_v1* thread = nullptr;
    auto* arg = new int(17);
    if (ores_native_thread_spawn_v1(entry, arg, destroy, &thread) !=
        ORES_THREAD_OK_V1) {
        delete arg;
        return {};
    }
    auto completion = reaper.submit(thread);
    if (!completion) {
        int result = 0;
        assert(ores_native_thread_join_blocking_v1(thread, &result) == ORES_THREAD_OK_V1);
        assert(ores_native_thread_release_v1(thread) == ORES_THREAD_OK_V1);
    }
    return completion;
}
int main() {
    using State = NativeJoinTaskWaitV1::State;
    assert(!NativeJoinTaskWaitV1::create(0, [](std::uint64_t) {}));
    assert(!NativeJoinTaskWaitV1::create(1, {}));
    std::atomic<int> enqueues{0};
    oreslang::runtime::NativeThreadJoinReaperV1 reaper(8, 2);
    for (int i = 0; i < 200; ++i) {
        auto complete = start(reaper);
        assert(complete);
        auto waiter = NativeJoinTaskWaitV1::create(
            static_cast<std::uint64_t>(i + 1),
            [&](std::uint64_t generation) {
                assert(generation != 0);
                enqueues.fetch_add(1, std::memory_order_relaxed);
            });
        assert(waiter && waiter->attach(complete));
        assert(!waiter->attach(complete));
        std::thread cancel_thread([waiter] { waiter->cancel(); });
        cancel_thread.join();
        // Completion callback and cancellation may race, but enqueue
        // is at most once, regardless of physical pthread completion.
        ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
        int value = 0;
        while (!complete->try_read(status, value)) std::this_thread::yield();
        assert(status == ORES_THREAD_OK_V1 && value == 17);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (waiter->state() == State::Pending) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
    assert(enqueues.load() <= 200);
    // A cancelled waiter must not execute a late scheduler enqueue.
    auto late = NativeJoinTaskWaitV1::create(9876, [&](std::uint64_t) {
        enqueues.fetch_add(1000);
    });
    assert(late && late->cancel());
    auto complete = start(reaper);
    assert(complete && late->attach(complete));
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int value = 0;
    while (!complete->try_read(status, value)) std::this_thread::yield();
    assert(late->state() == State::Cancelled);
    assert(enqueues.load() <= 200);
    std::cout << "native join task wait notification: OK (host-only)\n";
}
