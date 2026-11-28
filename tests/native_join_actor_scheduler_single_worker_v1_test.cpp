#include "oreslang/runtime/NativeJoinActorSchedulerWakeV1.hpp"
#include "oreslang/runtime/NativeThreadJoinReaperV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

using namespace oreslang::runtime;

namespace {
struct Gate final {
    std::atomic<int> state{0};  // pending, ready, cancelled
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

struct ThreadArg {
    std::atomic<bool>* release;
    std::atomic<int>* drops;
};
int native_entry(void* raw) {
    auto* arg = static_cast<ThreadArg*>(raw);
    while (!arg->release->load(std::memory_order_acquire))
        std::this_thread::yield(); // Dedicated native test pthread ONLY.
    return 73;
}
void native_drop(void* raw) {
    auto* arg = static_cast<ThreadArg*>(raw);
    arg->drops->fetch_add(1, std::memory_order_acq_rel);
    delete arg;
}

struct Scenario {
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    std::unique_ptr<NativeThreadJoinReaperV1> reaper{
        std::make_unique<NativeThreadJoinReaperV1>(1, 1)};
    std::shared_ptr<NativeJoinCompletionV1> completion;
    std::shared_ptr<NativeJoinActorSchedulerWakeV1> queue;
    std::shared_ptr<Gate> gate;
    std::atomic<int> phase{0};
    std::atomic<bool> release{false};
    std::atomic<int> drops{0};
    std::mutex lock;
    std::condition_variable cv;
    bool resumed = false;
    bool cancel_before_completion = false;
};

void on_message(void* raw, size_t actor_index, uint64_t payload) {
    auto& s = *static_cast<Scenario*>(raw);
    assert(actor_index == 0);
    assert(ores_native_actor_scheduler_is_current_carrier(s.scheduler));
    assert(ores_native_actor_scheduler_is_current_actor(
        s.scheduler, s.actor));
    if (payload == 1) {
        assert(s.phase.load(std::memory_order_acquire) == 0);
        // The Task's continuation/descriptor would supply these immutable
        // compiler-minted generations in a guest implementation.
        OresNativeWakeTicket ticket{};
        assert(ores_native_actor_scheduler_reserve_wake(
            s.scheduler, s.actor, s.actor, 11, 7, 5, 3, &ticket));
        assert(ores_native_actor_scheduler_transfer_wake(
            s.scheduler, ticket));
        s.queue = NativeJoinActorSchedulerWakeV1::create(
            s.scheduler, ticket);
        assert(s.queue);
        s.gate = std::make_shared<Gate>();

        auto* arg = new ThreadArg{&s.release, &s.drops};
        ores_native_thread_v1* handle = nullptr;
        auto status = ores_native_thread_spawn_v1(
            native_entry, arg, native_drop, &handle);
        if (status != ORES_THREAD_OK_V1) delete arg;
        assert(status == ORES_THREAD_OK_V1 && handle);
        s.completion = s.reaper->submit(handle);
        assert(s.completion && !handle);
        assert(NativeThreadJoinSchedulerWakeV1::subscribe(
            s.completion, std::weak_ptr<Gate>(s.gate),
            std::weak_ptr<NativeJoinSchedulerWakeQueueV1>(s.queue)));

        // The original Task's turn returns to the only carrier.
        // The carrier must execute payload 2 while pthread is gated.
        s.phase.store(1, std::memory_order_release);
        if (!s.cancel_before_completion) {
            assert(ores_native_actor_scheduler_try_send(
                s.scheduler, s.actor, 2) == ORES_NATIVE_SEND_ACCEPTED);
        }
        // In the cancellation case the HOST explicitly submits the second
        // turn after retiring the wake credit, avoiding a timing-dependent
        // race between setup and pthread completion.
        return;
    }
    assert(payload == 2);
    assert(s.phase.load(std::memory_order_acquire) == 1);
    assert(!s.release.load(std::memory_order_acquire));
    s.phase.store(2, std::memory_order_release);
    s.release.store(true, std::memory_order_release);
}

void on_wake(void* raw, size_t actor_index, OresNativeWakeTicket ticket,
             OresNativeWakeOutcome outcome) {
    auto& s = *static_cast<Scenario*>(raw);
    assert(!s.cancel_before_completion);
    assert(actor_index == 0 && outcome == ORES_NATIVE_WAKE_READY);
    assert(ores_native_actor_scheduler_is_current_wake(
        s.scheduler, ticket));
    // This proves the sole carrier ran OTHER work while join was pending.
    assert(s.phase.load(std::memory_order_acquire) == 2);
    assert(s.gate->state.load(std::memory_order_acquire) == 1);
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int result = 0;
    assert(s.completion->try_read(status, result));
    assert(status == ORES_THREAD_OK_V1 && result == 73);
    s.phase.store(3, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(s.lock);
        assert(!s.resumed);
        s.resumed = true;
    }
    s.cv.notify_one();
}

void host_wait_for_phase(Scenario& s, int required) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (s.phase.load(std::memory_order_acquire) < required) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();  // Test host, not scheduler carrier.
    }
}

void run(bool cancel_before_completion) {
    Scenario s;
    s.cancel_before_completion = cancel_before_completion;
    s.scheduler = ores_native_actor_scheduler_open_with_wakes(
        1, 3, 1, on_message, nullptr, nullptr, on_wake, &s);
    assert(s.scheduler);
    s.actor = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    assert(s.actor.scheduler_domain != 0);
    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, 1) == ORES_NATIVE_SEND_ACCEPTED);
    host_wait_for_phase(s, 1);

    if (cancel_before_completion) {
        s.gate->cancel();
        assert(s.queue->retire());
        assert(ores_native_actor_scheduler_try_send(
            s.scheduler, s.actor, 2) == ORES_NATIVE_SEND_ACCEPTED);
    } else {
        std::unique_lock<std::mutex> guard(s.lock);
        assert(s.cv.wait_for(
            guard, std::chrono::seconds(5), [&] { return s.resumed; }));
        assert(s.phase.load(std::memory_order_acquire) == 3);
    }

    // Native cleanup is host-only; never join on the scheduler carrier.
    host_wait_for_phase(s, 2);
    s.reaper.reset();
    assert(s.drops.load(std::memory_order_acquire) == 1);
    if (cancel_before_completion) {
        assert(s.gate->state.load(std::memory_order_acquire) == 2);
        assert(s.phase.load(std::memory_order_acquire) == 2);
        assert(s.queue->publish_status() == ORES_NATIVE_WAKE_INVALID);
        assert(ores_native_actor_scheduler_wakes_completed(s.scheduler) == 0);
    } else {
        assert(ores_native_actor_scheduler_wakes_completed(s.scheduler) <= 1);
        assert(s.queue->publish_status() == ORES_NATIVE_WAKE_PUBLISHED);
    }
    // The wake callback signals the host BEFORE the scheduler decrements
    // its own wake loan. Drain is the required physical-turn quiescence
    // fence; an immediate outstanding-credit assertion would race cleanup.
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_outstanding_wake_loans(
        s.scheduler, s.actor) == 0);
    s.queue.reset();
    s.gate.reset();
    s.completion.reset();

    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actor));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actor));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    if (!cancel_before_completion)
        assert(ores_native_actor_scheduler_wakes_completed(s.scheduler) == 1);
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
}
}  // namespace

int main() {
    for (int iteration = 0; iteration < 16; ++iteration) {
        run(false);
        run(true);
    }
    std::puts("NATIVE_JOIN_REAL_ONE_WORKER_SCHEDULER_HOST_ONLY_PASS");
}
