#include "oreslang/runtime/TaskContinuationGate.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

using oreslang::runtime::TaskCancellation;
using oreslang::runtime::TaskContinuationGate;

struct Stores {
    OresNativeExecutionContextStore* executions =
        ores_native_execution_context_store_open(8);
    OresNativeContinuationStore* continuations =
        ores_native_continuation_store_open(8, executions);

    Stores() {
        assert(executions && continuations);
    }
    ~Stores() {
        assert(ores_native_continuation_store_live(continuations) == 0);
        assert(ores_native_execution_context_store_live(executions) == 0);
        assert(ores_native_continuation_store_close(continuations));
        assert(ores_native_execution_context_store_close(executions));
    }

    OresNativeSuspensionLink suspend(std::uint64_t id) {
        OresNativeExecutionContext context{};
        context.id.runtime_domain = 88;
        context.id.execution = id;
        context.allocation.kind = ORES_ALLOCATION_DOMAIN_ACTOR;
        context.allocation.domain = 88;
        context.allocation.owner = 3;
        context.allocation.generation = 4;
        OresNativeExecutionHandle origin{};
        assert(ores_native_execution_context_store_insert(
            executions, &context, &origin));
        OresNativeSuspensionLink link{};
        assert(ores_native_await_trace_suspend(
            executions, continuations, origin,
            OresNativeContinuationHandle{},
            ORES_CONTINUATION_BOUNDARY_AWAIT, 11, id, &link));
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

static OresNativeExecutionId physical(std::uint64_t id) {
    return OresNativeExecutionId{88, id};
}

static void revokes_pending_and_drains_children() {
    Stores stores;
    auto parent = TaskCancellation::root();
    auto child = parent->child();
    const auto link = stores.suspend(100);
    int quiesced = 0;
    assert(!parent->seal_and_observe_child_quiescence(
        [&] { ++quiesced; }));
    {
        TaskContinuationGate gate(child, stores.executions,
                                  stores.continuations, link);
        assert(parent->cancel("parent-shutdown"));
        assert(child->is_cancelled());
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        assert(!gate.notify_ready());
        OresNativeExecutionHandle resumed{};
        assert(!gate.try_resume(physical(101), &resumed));
        assert(!gate.finish_resume());
        gate.close();
    }
    stores.release(link);
    assert(quiesced == 0);
    child->retire();
    assert(quiesced == 1);
    parent->retire();
}

static void late_task_cancel_cannot_reenter_claimed_guest() {
    Stores stores;
    auto parent = TaskCancellation::root();
    auto child = parent->child();
    const auto link = stores.suspend(200);
    {
        TaskContinuationGate gate(child, stores.executions,
                                  stores.continuations, link);
        OresNativeExecutionHandle resumed{};
        OresNativeExecutionContext context{};
        assert(!gate.try_resume(physical(201), &resumed));
        assert(gate.notify_ready());
        assert(!gate.notify_ready());
        assert(gate.try_resume(physical(201), &resumed, &context));
        assert(context.id.execution == 201);
        assert(context.allocation.owner == 3);
        assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
        assert(!gate.try_resume(physical(202), &resumed));
        assert(parent->cancel("late-cancellation"));
        assert(child->is_cancelled());
        // Cancellation after claim must be handled cooperatively by guest code.
        assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
        assert(gate.finish_resume());
        assert(!gate.finish_resume());
        assert(ores_native_execution_context_store_release(
            stores.executions, resumed));
    }
    stores.release(link);
    child->retire();
    parent->retire();
}

static void already_cancelled_authority_closes_registration_race() {
    Stores stores;
    auto parent = TaskCancellation::root();
    assert(parent->cancel("prior"));
    auto child = parent->child();
    const auto link = stores.suspend(300);
    {
        TaskContinuationGate gate(child, stores.executions,
                                  stores.continuations, link);
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        assert(!gate.notify_ready());
        OresNativeExecutionHandle resumed{};
        assert(!gate.try_resume(physical(301), &resumed));
    }
    stores.release(link);
    child->retire();
    parent->retire();
}

static void rejects_forged_and_recycled_await_links() {
    Stores stores;
    auto task = TaskCancellation::root();
    const auto link = stores.suspend(400);
    OresNativeSuspensionLink forged = link;
    forged.execution_id.execution += 1;
    bool rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, forged);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    forged = link;
    forged.parent.generation += 1;
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, forged);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link);
        gate.close();
    }
    stores.release(link);
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    task->retire();
}

static void completion_cancel_and_resume_cancel_races() {
    Stores stores;
    for (std::uint64_t i = 0; i < 256; ++i) {
        {
            auto parent = TaskCancellation::root();
            auto child = parent->child();
            const auto link = stores.suspend(1000 + i);
            TaskContinuationGate gate(child, stores.executions,
                                      stores.continuations, link);
            std::atomic<bool> go{false};
            std::atomic<bool> accepted{false};
            std::thread producer([&] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                accepted.store(gate.notify_ready(), std::memory_order_release);
            });
            std::thread canceller([&] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                assert(parent->cancel("race"));
            });
            go.store(true, std::memory_order_release);
            producer.join();
            canceller.join();
            (void)accepted;
            assert(child->is_cancelled());
            assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
            OresNativeExecutionHandle resumed{};
            assert(!gate.try_resume(physical(3000 + i), &resumed));
            gate.close();
            stores.release(link);
            child->retire();
            parent->retire();
        }
        {
            auto parent = TaskCancellation::root();
            auto child = parent->child();
            const auto link = stores.suspend(4000 + i);
            TaskContinuationGate gate(child, stores.executions,
                                      stores.continuations, link);
            assert(gate.notify_ready());
            std::atomic<bool> go{false};
            std::atomic<bool> claimed{false};
            OresNativeExecutionHandle resumed{};
            std::thread resumer([&] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                claimed.store(
                    gate.try_resume(physical(6000 + i), &resumed),
                    std::memory_order_release);
            });
            std::thread canceller([&] {
                while (!go.load(std::memory_order_acquire))
                    std::this_thread::yield();
                assert(parent->cancel("race-after-ready"));
            });
            go.store(true, std::memory_order_release);
            resumer.join();
            canceller.join();

            if (claimed.load(std::memory_order_acquire)) {
                assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
                assert(gate.finish_resume());
                assert(ores_native_execution_context_store_release(
                    stores.executions, resumed));
            } else {
                assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
                assert(!gate.finish_resume());
            }
            assert(child->is_cancelled());
            gate.close();
            stores.release(link);
            child->retire();
            parent->retire();
        }
    }
}

static void close_disarms_concurrent_revocation() {
    Stores stores;
    for (std::uint64_t i = 0; i < 128; ++i) {
        auto parent = TaskCancellation::root();
        auto child = parent->child();
        const auto link = stores.suspend(7000 + i);
        TaskContinuationGate gate(child, stores.executions,
                                  stores.continuations, link);
        std::thread cancellation([&] { (void)parent->cancel("shutdown"); });
        gate.close();
        cancellation.join();
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        stores.release(link);
        child->retire();
        parent->retire();
    }
}

int main() {
    revokes_pending_and_drains_children();
    late_task_cancel_cannot_reenter_claimed_guest();
    already_cancelled_authority_closes_registration_race();
    rejects_forged_and_recycled_await_links();
    completion_cancel_and_resume_cancel_races();
    close_disarms_concurrent_revocation();
    std::puts("NATIVE_TASK_CONTINUATION_GATE_PASS");
}
