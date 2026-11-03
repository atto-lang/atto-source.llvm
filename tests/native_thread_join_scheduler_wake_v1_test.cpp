#include "oreslang/runtime/NativeThreadJoinSchedulerWakeV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <thread>

using namespace oreslang::runtime;

namespace {
struct Gate final {
    // 0 pending, 1 ready, 2 cancelled/retired.
    std::atomic<int> state{0};
    bool notify_ready() noexcept {
        int expected = 0;
        return state.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel);
    }
    void cancel() noexcept {
        int expected = 0;
        (void)state.compare_exchange_strong(
            expected, 2, std::memory_order_acq_rel);
    }
};
struct Queue final : NativeJoinSchedulerWakeQueueV1 {
    std::mutex mutex;
    std::condition_variable cv;
    int wakes = 0;
    void enqueue_reserved_ready() noexcept override {
        std::lock_guard<std::mutex> lock(mutex);
        ++wakes;
        cv.notify_all();
    }
    bool await_count(int target) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5),
                           [&] { return wakes >= target; });
    }
    int count() {
        std::lock_guard<std::mutex> lock(mutex);
        return wakes;
    }
};
struct OwnedArg {
    int result;
    std::atomic<bool>* release;
};
std::atomic<int> drops{0};
int entry(void* context) {
    auto* arg = static_cast<OwnedArg*>(context);
    while (arg->release &&
           !arg->release->load(std::memory_order_acquire))
        std::this_thread::yield(); // test's native worker, not scheduler carrier
    return arg->result;
}
void destroy(void* context) {
    delete static_cast<OwnedArg*>(context);
    drops.fetch_add(1);
}
ores_native_thread_v1* spawn(int value, std::atomic<bool>* release) {
    auto* arg = new OwnedArg{value, release};
    ores_native_thread_v1* handle = nullptr;
    auto status = ores_native_thread_spawn_v1(entry, arg, destroy, &handle);
    if (status != ORES_THREAD_OK_V1) delete arg;
    assert(status == ORES_THREAD_OK_V1 && handle);
    return handle;
}
void await_join(const std::shared_ptr<NativeJoinCompletionV1>& completion,
                int expected) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int value = -1;
    // Only test harness observes a host completion; production has no polling.
    while (!completion->try_read(status, value)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    assert(status == ORES_THREAD_OK_V1 && value == expected);
}
void test_before_and_after_join() {
    NativeThreadJoinReaperV1 reaper(2, 2);
    std::atomic<bool> release{false};
    auto* first = spawn(501, &release);
    auto completion = reaper.submit(first);
    assert(completion && !first);
    auto gate = std::make_shared<Gate>();
    auto queue = std::make_shared<Queue>();
    assert(NativeThreadJoinSchedulerWakeV1::subscribe(
        completion, std::weak_ptr<Gate>(gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(queue)));
    assert(!NativeThreadJoinSchedulerWakeV1::subscribe(
        completion, std::weak_ptr<Gate>(gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(queue)));
    assert(queue->count() == 0);
    release.store(true, std::memory_order_release);
    assert(queue->await_count(1));  // event-driven; no polling on scheduler
    await_join(completion, 501);
    assert(gate->state.load() == 1 && queue->count() == 1);

    auto* second = spawn(502, nullptr);
    auto done = reaper.submit(second);
    assert(done && !second);
    await_join(done, 502);
    auto late_gate = std::make_shared<Gate>();
    auto late_queue = std::make_shared<Queue>();
    assert(NativeThreadJoinSchedulerWakeV1::subscribe(
        done, std::weak_ptr<Gate>(late_gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(late_queue)));
    assert(late_queue->await_count(1));
    assert(!NativeThreadJoinSchedulerWakeV1::subscribe(
        done, std::weak_ptr<Gate>(late_gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(late_queue)));
}
void test_cancel_and_retirement() {
    NativeThreadJoinReaperV1 reaper(2, 2);
    std::atomic<bool> release{false};
    auto* native = spawn(601, &release);
    auto completion = reaper.submit(native);
    auto gate = std::make_shared<Gate>();
    auto queue = std::make_shared<Queue>();
    assert(NativeThreadJoinSchedulerWakeV1::subscribe(
        completion, std::weak_ptr<Gate>(gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(queue)));
    gate->cancel(); // no authority left to queue a wake
    release.store(true, std::memory_order_release);
    await_join(completion, 601);
    assert(gate->state.load() == 2 && queue->count() == 0);

    // Hold the second native worker until its logical gate has retired.
    // Without this gate the thread can finish and legitimately notify first.
    release.store(false, std::memory_order_release);
    auto* second = spawn(602, &release);
    auto other = reaper.submit(second);
    auto stale_gate = std::make_shared<Gate>();
    assert(NativeThreadJoinSchedulerWakeV1::subscribe(
        other, std::weak_ptr<Gate>(stale_gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(queue)));
    stale_gate.reset(); // generation retired; reaper keeps no strong ref
    release.store(true, std::memory_order_release);
    await_join(other, 602);
    assert(queue->count() == 0);
    assert(!NativeThreadJoinSchedulerWakeV1::subscribe(
        nullptr, std::weak_ptr<Gate>(gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(queue)));
}
void test_queue_retirement_drops_late_wake() {
    NativeThreadJoinReaperV1 reaper(1, 1);
    std::atomic<bool> release{false};
    auto* native = spawn(803, &release);
    auto completion = reaper.submit(native);
    assert(completion && !native);
    auto gate = std::make_shared<Gate>();
    auto queue = std::make_shared<Queue>();
    const std::weak_ptr<Queue> weak_queue = queue;
    assert(NativeThreadJoinSchedulerWakeV1::subscribe(
        completion, std::weak_ptr<Gate>(gate),
        std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(queue)));
    // The native join must proceed despite scheduler retirement, without
    // acquiring a strong reference to the retired queue or waking a Task.
    queue.reset();
    assert(weak_queue.expired());
    release.store(true, std::memory_order_release);
    await_join(completion, 803);
    assert(gate->state.load(std::memory_order_acquire) == 0);
}
void test_completion_cancel_race() {
    NativeThreadJoinReaperV1 reaper(8, 2);
    for (int i = 0; i < 64; ++i) {
        std::atomic<bool> release{false};
        auto* native = spawn(700 + i, &release);
        auto completion = reaper.submit(native);
        assert(completion && !native);
        auto gate = std::make_shared<Gate>();
        auto queue = std::make_shared<Queue>();
        assert(NativeThreadJoinSchedulerWakeV1::subscribe(
            completion, std::weak_ptr<Gate>(gate),
            std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(queue)));
        std::thread canceller([gate, &release] {
            release.store(true, std::memory_order_release);
            gate->cancel();
        });
        canceller.join();
        await_join(completion, 700 + i);
        const int state = gate->state.load();
        assert(state == 1 || state == 2);
        if (state == 1) assert(queue->await_count(1));
        else assert(queue->count() == 0);
        assert(queue->count() <= 1); // never double resume
    }
}
} // namespace

int main() {
    const int baseline = drops.load();
    test_before_and_after_join();
    test_cancel_and_retirement();
    test_queue_retirement_drops_late_wake();
    test_completion_cancel_race();
    assert(drops.load() == baseline + 69);
}
