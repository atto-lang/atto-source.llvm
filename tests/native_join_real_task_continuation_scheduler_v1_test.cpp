#include "oreslang/runtime/NativeJoinActorSchedulerWakeV1.hpp"
#include "oreslang/runtime/NativeJoinTaskContinuationBridgeV1.hpp"

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
struct Stores {
    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(16);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(16, executions);

    Stores() { assert(executions && continuations); }
    ~Stores() {
        assert(ores_native_continuation_store_live(continuations) == 0);
        assert(ores_native_execution_context_store_live(executions) == 0);
        assert(ores_native_continuation_store_close(continuations));
        assert(ores_native_execution_context_store_close(executions));
    }

    OresNativeSuspensionLink suspend(OresNativeExecutionContext ctx) {
        OresNativeExecutionHandle origin{};
        assert(ores_native_execution_context_store_insert(
            executions, &ctx, &origin));
        OresNativeSuspensionLink link{};
        assert(ores_native_await_trace_suspend(
            executions, continuations, origin,
            OresNativeContinuationHandle{},
            ORES_CONTINUATION_BOUNDARY_TASK_JOIN, 11, 1, &link));
        assert(ores_native_execution_context_store_release(
            executions, origin));
        return link;
    }

    void release(OresNativeSuspensionLink link) {
        assert(ores_native_continuation_store_release(
            continuations, link.continuation));
    }
};

struct ThreadArg {
    std::atomic<bool>* release;
    std::atomic<int>* drops;
};
int native_entry(void* raw) {
    auto* arg = static_cast<ThreadArg*>(raw);
    while (!arg->release->load(std::memory_order_acquire))
        std::this_thread::yield(); // native pthread, never a scheduler carrier
    return 83;
}
void native_drop(void* raw) {
    auto* arg = static_cast<ThreadArg*>(raw);
    arg->drops->fetch_add(1, std::memory_order_acq_rel);
    delete arg;
}

struct Scenario {
    Stores stores;
    OresNativeActorScheduler* scheduler = nullptr;
    OresNativeActorRef actor{};
    OresNativeSuspensionLink link{};
    TaskCancellation::Ptr authority{TaskCancellation::root()};
    std::shared_ptr<TaskContinuationGate> gate;
    std::shared_ptr<NativeJoinTaskContinuationBridgeV1> bridge;
    std::shared_ptr<NativeJoinActorSchedulerWakeV1> queue;
    std::unique_ptr<NativeThreadJoinReaperV1> reaper{
        std::make_unique<NativeThreadJoinReaperV1>(1, 1)};
    std::shared_ptr<NativeJoinCompletionV1> completion;
    OresNativeAllocationDomainRef expected_allocation{};
    std::atomic<bool> release_native{false};
    std::atomic<int> drops{0};
    std::atomic<int> phase{0};
    bool cancel_before_completion = false;
    bool resumed = false;
    std::mutex done_mutex;
    std::condition_variable done_cv;
};

void on_message(void* raw, size_t lane, uint64_t payload) {
    auto& s = *static_cast<Scenario*>(raw);
    assert(lane == 0);
    assert(ores_native_actor_scheduler_is_current_actor(
        s.scheduler, s.actor));
    OresNativeExecutionContext physical{};
    assert(ores_native_actor_scheduler_current_execution_context(
        s.scheduler, &physical));
    if (payload == 1) {
        assert(s.phase.load(std::memory_order_acquire) == 0);
        s.expected_allocation = physical.allocation;
        s.link = s.stores.suspend(physical);
        s.gate = std::make_shared<TaskContinuationGate>(
            s.authority, s.stores.executions, s.stores.continuations,
            s.link, 11, s.expected_allocation,
            ORES_CONTINUATION_BOUNDARY_TASK_JOIN);

        OresNativeWakeTicket ticket{};
        assert(ores_native_actor_scheduler_reserve_wake(
            s.scheduler, s.actor, s.actor,
            static_cast<uint64_t>(s.link.continuation.slot) + 1,
            s.link.continuation.generation, 11, 3, &ticket));
        assert(ores_native_actor_scheduler_transfer_wake(
            s.scheduler, ticket));
        s.queue = NativeJoinActorSchedulerWakeV1::create(
            s.scheduler, ticket);
        assert(s.queue);

        auto* arg = new ThreadArg{&s.release_native, &s.drops};
        ores_native_thread_v1* handle = nullptr;
        const auto result = ores_native_thread_spawn_v1(
            native_entry, arg, native_drop, &handle);
        if (result != ORES_THREAD_OK_V1) delete arg;
        assert(result == ORES_THREAD_OK_V1 && handle);
        s.completion = s.reaper->submit(handle);
        assert(s.completion && !handle);
        const std::weak_ptr<NativeJoinActorSchedulerWakeV1> weak_queue =
            s.queue;
        s.bridge = NativeJoinTaskContinuationBridgeV1::subscribe(
            s.completion, s.gate, 31,
            [weak_queue](std::uint64_t generation) {
                // Called ONLY on the native reaper. This is an enqueue
                // operation, never continuation claim or guest execution.
                assert(generation == 31);
                if (auto queue = weak_queue.lock())
                    queue->enqueue_reserved_ready();
            });
        assert(s.bridge);

        // Leave the one carrier available for the second logical turn.
        s.phase.store(1, std::memory_order_release);
        if (!s.cancel_before_completion)
            assert(ores_native_actor_scheduler_try_send(
                s.scheduler, s.actor, 2) == ORES_NATIVE_SEND_ACCEPTED);
        return;
    }
    assert(payload == 2);
    assert(s.phase.load(std::memory_order_acquire) == 1);
    s.phase.store(2, std::memory_order_release);
    s.release_native.store(true, std::memory_order_release);
}

void on_wake(void* raw, size_t lane, OresNativeWakeTicket ticket,
             OresNativeWakeOutcome outcome) {
    auto& s = *static_cast<Scenario*>(raw);
    assert(lane == 0 && outcome == ORES_NATIVE_WAKE_READY);
    assert(!s.cancel_before_completion);
    assert(ores_native_actor_scheduler_is_current_wake(
        s.scheduler, ticket));
    assert(s.phase.load(std::memory_order_acquire) == 2);
    assert(s.bridge->try_admit_ready(31));
    assert(!s.bridge->try_admit_ready(32));
    OresNativeExecutionContext physical{};
    assert(ores_native_actor_scheduler_current_execution_context(
        s.scheduler, &physical));
    OresNativeExecutionHandle resumed{};
    assert(s.gate->try_resume(physical.id, &resumed));
    assert(!s.gate->try_resume(physical.id, &resumed));
    assert(s.gate->finish_resume());
    assert(!s.gate->finish_resume());
    assert(ores_native_execution_context_store_release(
        s.stores.executions, resumed));
    ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
    int output = 0;
    assert(s.completion->try_read(status, output));
    assert(status == ORES_THREAD_OK_V1 && output == 83);
    s.phase.store(3, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(s.done_mutex);
        assert(!s.resumed);
        s.resumed = true;
    }
    s.done_cv.notify_one();
}

void wait_phase(Scenario& s, int phase) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(8);
    while (s.phase.load(std::memory_order_acquire) < phase) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield(); // test host only
    }
}

void run(bool cancelled) {
    Scenario s;
    s.cancel_before_completion = cancelled;
    s.scheduler = ores_native_actor_scheduler_open_with_wakes(
        1, 3, 1, on_message, nullptr, nullptr, on_wake, &s);
    assert(s.scheduler);
    s.actor = ores_native_actor_scheduler_spawn(s.scheduler, 0);
    assert(s.actor.scheduler_domain);
    assert(ores_native_actor_scheduler_try_send(
        s.scheduler, s.actor, 1) == ORES_NATIVE_SEND_ACCEPTED);
    wait_phase(s, 1);

    if (cancelled) {
        assert(s.authority->cancel("join-wait-cancelled"));
        assert(s.bridge->cancel_waiter());
        assert(s.queue->retire());
        assert(ores_native_actor_scheduler_try_send(
            s.scheduler, s.actor, 2) == ORES_NATIVE_SEND_ACCEPTED);
    } else {
        std::unique_lock<std::mutex> guard(s.done_mutex);
        assert(s.done_cv.wait_for(
            guard, std::chrono::seconds(8), [&] { return s.resumed; }));
        assert(s.phase.load(std::memory_order_acquire) == 3);
    }
    wait_phase(s, 2);
    s.reaper.reset(); // host-only physical join/reaper quiescence
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(s.drops.load(std::memory_order_acquire) == 1);
    assert(ores_native_actor_scheduler_outstanding_wake_loans(
        s.scheduler, s.actor) == 0);
    if (cancelled) {
        assert(!s.bridge->try_admit_ready(31));
        OresNativeExecutionHandle invalid{};
        assert(!s.gate->try_resume({s.actor.scheduler_domain, 1200},
                                   &invalid));
        assert(ores_native_actor_scheduler_wakes_completed(
            s.scheduler) == 0);
    } else {
        assert(ores_native_actor_scheduler_wakes_completed(
            s.scheduler) == 1);
    }
    s.bridge.reset();
    s.queue.reset();
    s.gate->close();
    s.gate.reset();
    s.stores.release(s.link);
    s.authority->retire();

    assert(ores_native_actor_scheduler_stop(s.scheduler, s.actor));
    assert(ores_native_actor_scheduler_wait_done(s.scheduler, s.actor));
    ores_native_actor_scheduler_retire(s.scheduler);
    assert(ores_native_actor_scheduler_drain(s.scheduler));
    assert(ores_native_actor_scheduler_try_destroy(s.scheduler));
}
} // namespace

int main() {
    for (int iteration = 0; iteration < 8; ++iteration) {
        run(false);
        run(true);
    }
    std::puts("NATIVE_JOIN_REAL_TASK_CONTINUATION_ONE_WORKER_HOST_ONLY_PASS");
}
