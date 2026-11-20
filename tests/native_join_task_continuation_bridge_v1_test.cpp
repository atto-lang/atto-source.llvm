#include "oreslang/runtime/NativeJoinTaskContinuationBridgeV1.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using oreslang::runtime::NativeJoinTaskContinuationBridgeV1;
using oreslang::runtime::NativeThreadJoinReaperV1;
using oreslang::runtime::TaskCancellation;
using oreslang::runtime::TaskContinuationGate;
using oreslang::runtime::NativeJoinCompletionV1;

struct Stores {
    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(8);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(8, executions);
    Stores() { assert(executions && continuations); }
    ~Stores() {
        assert(ores_native_continuation_store_live(continuations) == 0);
        assert(ores_native_execution_context_store_live(executions) == 0);
        assert(ores_native_continuation_store_close(continuations));
        assert(ores_native_execution_context_store_close(executions));
    }
    OresNativeSuspensionLink suspend(std::uint64_t id) {
        OresNativeExecutionContext ctx{};
        ctx.id = {88, id};
        ctx.allocation = {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4};
        OresNativeExecutionHandle origin{};
        assert(ores_native_execution_context_store_insert(
            executions, &ctx, &origin));
        OresNativeSuspensionLink link{};
        assert(ores_native_await_trace_suspend(
            executions, continuations, origin,
            OresNativeContinuationHandle{},
            ORES_CONTINUATION_BOUNDARY_TASK_JOIN, 11, id, &link));
        assert(ores_native_execution_context_store_release(executions, origin));
        return link;
    }
    void release(OresNativeSuspensionLink link) {
        assert(ores_native_continuation_store_release(
            continuations, link.continuation));
    }
    OresNativeContinuationState state(OresNativeSuspensionLink link) {
        OresNativeContinuationRecord record{};
        assert(ores_native_continuation_store_get(
            continuations, link.continuation, &record));
        return record.state;
    }
};
struct Arg { std::atomic<bool>* gate; };
static int native_entry(void* raw) {
    auto* arg = static_cast<Arg*>(raw);
    while (!arg->gate->load(std::memory_order_acquire))
        std::this_thread::yield(); // test-owned native worker only
    return 67;
}
static void native_drop(void* raw) { delete static_cast<Arg*>(raw); }
static std::shared_ptr<NativeJoinCompletionV1> start(
        NativeThreadJoinReaperV1& reaper, std::atomic<bool>& gate) {
    ores_native_thread_v1* handle = nullptr;
    auto* arg = new Arg{&gate};
    if (ores_native_thread_spawn_v1(native_entry, arg, native_drop, &handle)
            != ORES_THREAD_OK_V1) {
        delete arg;
        return {};
    }
    auto completion = reaper.submit(handle);
    if (!completion) {
        int result = 0;
        assert(ores_native_thread_join_blocking_v1(handle, &result) == ORES_THREAD_OK_V1);
        assert(ores_native_thread_release_v1(handle) == ORES_THREAD_OK_V1);
    }
    return completion;
}
template <class P>
static void until(P predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(8);
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
static void observed_join(const std::shared_ptr<NativeJoinCompletionV1>& done) {
    until([&] {
        ores_thread_status_v1 status = ORES_THREAD_OS_ERROR_V1;
        int value = 0;
        if (!done->try_read(status, value)) return false;
        assert(status == ORES_THREAD_OK_V1 && value == 67);
        return true;
    });
}
static std::shared_ptr<TaskContinuationGate> bind_task(
        Stores& stores, const TaskCancellation::Ptr& authority,
        OresNativeSuspensionLink link) {
    return std::make_shared<TaskContinuationGate>(
        authority, stores.executions, stores.continuations,
        link, 11, OresNativeAllocationDomainRef{
            ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4},
        ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
}

static void nonblocking_join_publishes_one_ready_ticket() {
    Stores stores;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::vector<std::uint64_t> tickets;
    auto reaper = std::make_unique<NativeThreadJoinReaperV1>(2, 2);
    auto authority = TaskCancellation::root();
    auto link = stores.suspend(2001);
    auto task = bind_task(stores, authority, link);
    std::atomic<bool> native_gate{false};
    auto completion = start(*reaper, native_gate);
    assert(completion);
    auto waiter = NativeJoinTaskContinuationBridgeV1::subscribe(
        completion, task, 3001, [&](std::uint64_t generation) {
            std::lock_guard<std::mutex> guard(queue_mutex);
            tickets.push_back(generation); // no guest execution on reaper
            queue_cv.notify_one();
        });
    assert(waiter);
    assert(!NativeJoinTaskContinuationBridgeV1::subscribe(
        completion, task, 3002, [](std::uint64_t) {}));
    {
        std::lock_guard<std::mutex> guard(queue_mutex);
        assert(tickets.empty()); // native worker is gated, no premature wake
    }
    native_gate.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> guard(queue_mutex);
        assert(queue_cv.wait_for(
            guard, std::chrono::seconds(8), [&] { return !tickets.empty(); }));
        assert(tickets.size() == 1 && tickets.front() == 3001);
    }
    // The reaper only enqueues. Logical readiness is published by the
    // scheduler's authenticated ticket consumer, never by the reaper.
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);
    assert(!waiter->try_admit_ready(3002)); // wrong incarnation
    assert(waiter->try_admit_ready(3001));
    assert(!waiter->try_admit_ready(3001)); // one-shot state transition
    assert(stores.state(link) == ORES_CONTINUATION_READY);
    OresNativeExecutionHandle resumed{};
    assert(task->try_resume({88, 2002}, &resumed)); // scheduler-only claim
    assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
    assert(!task->try_resume({88, 2003}, &resumed));
    assert(task->finish_resume());
    assert(!task->finish_resume());
    assert(ores_native_execution_context_store_release(stores.executions, resumed));
    observed_join(completion);
    // Host-only teardown barrier: finish the reaper's callback before
    // dropping the borrowed continuation stores or the queue lifetime.
    reaper.reset();
    task->close();
    task.reset();
    waiter.reset();
    stores.release(link);
    authority->retire();
}

static void already_joined_thread_registers_without_losing_wakeup() {
    Stores stores;
    auto reaper = std::make_unique<NativeThreadJoinReaperV1>(2, 2);
    auto authority = TaskCancellation::root();
    auto link = stores.suspend(3101);
    auto task = bind_task(stores, authority, link);
    std::atomic<bool> native_gate{true};
    auto completion = start(*reaper, native_gate);
    observed_join(completion); // force registration AFTER physical join
    std::atomic<int> notifications{0};
    auto bridge = NativeJoinTaskContinuationBridgeV1::subscribe(
        completion, task, 3102, [&](std::uint64_t g) {
            assert(g == 3102);
            notifications.fetch_add(1);
        });
    assert(bridge);
    assert(notifications.load() == 1); // callback ran inline
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);
    assert(bridge->try_admit_ready(3102));
    assert(!bridge->try_admit_ready(3102));
    OresNativeExecutionHandle resumed{};
    assert(task->try_resume({88, 3103}, &resumed));
    assert(task->finish_resume());
    assert(ores_native_execution_context_store_release(stores.executions, resumed));
    reaper.reset(); // callback quiescence precedes borrowed store release
    task->close();
    bridge.reset();
    task.reset();
    stores.release(link);
    authority->retire();
}

static void cancelled_task_never_enqueues_or_resumes() {
    Stores stores;
    std::atomic<int> enqueues{0};
    auto reaper = std::make_unique<NativeThreadJoinReaperV1>(2, 2);
    auto authority = TaskCancellation::root();
    auto link = stores.suspend(4001);
    auto task = bind_task(stores, authority, link);
    std::atomic<bool> native_gate{false};
    auto completion = start(*reaper, native_gate);
    assert(completion);
    auto waiter = NativeJoinTaskContinuationBridgeV1::subscribe(
        completion, task, 5001, [&](std::uint64_t) { enqueues.fetch_add(1); });
    assert(waiter);
    assert(authority->cancel("native-join-cancel"));
    assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
    native_gate.store(true, std::memory_order_release);
    observed_join(completion);
    // Host-only teardown barrier: finish the reaper's callback before
    // dropping the borrowed continuation stores or the queue lifetime.
    reaper.reset();
    OresNativeExecutionHandle resumed{};
    assert(!task->try_resume({88, 4002}, &resumed));
    // Queued cancellation tickets never authorize continuation readiness.
    assert(enqueues.load() <= 1);
    assert(!waiter->try_admit_ready(5001));
    task->close();
    task.reset();
    waiter.reset();
    stores.release(link);
    authority->retire();
}

static void retired_task_weak_gate_cannot_be_reanimated() {
    Stores stores;
    std::atomic<int> enqueues{0};
    auto reaper = std::make_unique<NativeThreadJoinReaperV1>(2, 2);
    auto authority = TaskCancellation::root();
    auto link = stores.suspend(6001);
    auto task = bind_task(stores, authority, link);
    std::atomic<bool> native_gate{false};
    auto completion = start(*reaper, native_gate);
    assert(completion);
    auto waiter = NativeJoinTaskContinuationBridgeV1::subscribe(
        completion, task, 7001, [&](std::uint64_t) { enqueues.fetch_add(1); });
    assert(waiter);
    task->close();
    task.reset(); // weak subscription cannot pin old borrowed C stores
    authority->retire();
    native_gate.store(true, std::memory_order_release);
    observed_join(completion);
    // Host-only teardown barrier: finish the reaper's callback before
    // dropping the borrowed continuation stores or the queue lifetime.
    reaper.reset();
    assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
    // Weak Task authority is gone. Any queued ticket must fail admission.
    assert(enqueues.load() <= 1);
    assert(!waiter->try_admit_ready(7001));
    waiter.reset();
    stores.release(link);
}
int main() {
    nonblocking_join_publishes_one_ready_ticket();
    already_joined_thread_registers_without_losing_wakeup();
    cancelled_task_never_enqueues_or_resumes();
    retired_task_weak_gate_cannot_be_reanimated();
    std::puts("NATIVE_JOIN_TASK_CONTINUATION_BRIDGE_HOST_ONLY_PASS");
}
