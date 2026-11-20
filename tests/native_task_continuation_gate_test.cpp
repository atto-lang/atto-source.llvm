#include "oreslang/runtime/TaskContinuationGate.hpp"
#if defined(__linux__)
#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>

using oreslang::runtime::TaskCancellation;
using oreslang::runtime::TaskContinuationGate;
#if defined(__linux__)
using oreslang::runtime::NativeActorGroup;
using oreslang::runtime::NativeEpollReactor;
using oreslang::runtime::NativeEpollReactorManager;
#endif

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

    OresNativeSuspensionLink suspend(
            std::uint64_t id,
            OresNativeContinuationBoundary boundary =
                ORES_CONTINUATION_BOUNDARY_AWAIT) {
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
            boundary, 11, id, &link));
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
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
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
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        OresNativeExecutionHandle resumed{};
        OresNativeExecutionContext context{};
        assert(!gate.try_resume(physical(201), &resumed));
        assert(gate.notify_ready());
        assert(!gate.notify_ready());
        assert(gate.try_resume(physical(201), &resumed, &context));
        assert(context.id.execution == 201);
        assert(context.allocation.owner == 3);
        assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
        // Final reference release must not retire the live guest frame.
        assert(!ores_native_continuation_store_release(
            stores.continuations, link.continuation));
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
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
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
    // An otherwise valid link must not cross reload or actor-owned heaps.
    bool wrong_code_rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link, 12,
                                  {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
    } catch (const std::invalid_argument&) {
        wrong_code_rejected = true;
    }
    assert(wrong_code_rejected);
    bool wrong_allocation_rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link, 11,
                                  {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 4, 4});
    } catch (const std::invalid_argument&) {
        wrong_allocation_rejected = true;
    }
    assert(wrong_allocation_rejected);
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    OresNativeSuspensionLink forged = link;
    forged.execution_id.execution += 1;
    bool rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, forged, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    forged = link;
    forged.parent.slot += 1;
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, forged, 11,
                                  {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    forged = link;
    forged.continuation.generation += 1;
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, forged, 11,
                                  {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
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
                                  stores.continuations, forged, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        gate.close();
    }
    stores.release(link);
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    task->retire();
}

static void rejects_non_await_control_and_spawn_boundaries() {
    Stores stores;
    auto task = TaskCancellation::root();
    for (const auto boundary : {
            ORES_CONTINUATION_BOUNDARY_SPAWN,
            ORES_CONTINUATION_BOUNDARY_SUPERVISOR_CONTROL,
            ORES_CONTINUATION_BOUNDARY_ACTOR_MESSAGE,
            ORES_CONTINUATION_BOUNDARY_TASK_JOIN}) {
        const auto link = stores.suspend(450 + static_cast<int>(boundary),
                                         boundary);
        bool rejected = false;
        try {
            TaskContinuationGate gate(task, stores.executions,
                                      stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
        assert(stores.state(link) == ORES_CONTINUATION_PENDING);
        assert(ores_native_continuation_store_cancel(
            stores.continuations, link.continuation));
        stores.release(link);
    }
    task->retire();
}

static void retirement_is_a_strict_waiter_revocation_barrier() {
    Stores stores;
    {
        auto task = TaskCancellation::root();
        const auto link = stores.suspend(500);
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        assert(gate.notify_ready());
        task->retire(); // Without Task.cancel(), still revoke READY!
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        OresNativeExecutionHandle resumed{};
        assert(!gate.try_resume(physical(501), &resumed));
        gate.close();
        stores.release(link);
    }
    {
        auto task = TaskCancellation::root();
        task->retire();
        const auto link = stores.suspend(502);
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        assert(!gate.notify_ready());
        gate.close();
        stores.release(link);
    }
    for (std::uint64_t i = 0; i < 128; ++i) {
        auto task = TaskCancellation::root();
        const auto link = stores.suspend(8000 + i);
        TaskContinuationGate gate(task, stores.executions,
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        std::atomic<bool> go{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)gate.notify_ready();
        });
        std::thread retire([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            task->retire();
        });
        go.store(true, std::memory_order_release);
        producer.join();
        retire.join();
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        OresNativeExecutionHandle resumed{};
        assert(!gate.try_resume(physical(9000 + i), &resumed));
        gate.close();
        stores.release(link);
    }
}

static void retirement_and_ready_resume_claim_are_linearized() {
    Stores stores;
    for (std::uint64_t i = 0; i < 128; ++i) {
        auto authority = TaskCancellation::root();
        const auto link = stores.suspend(10000 + i);
        TaskContinuationGate gate(authority, stores.executions,
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        assert(gate.notify_ready());
        OresNativeExecutionHandle resumed{};
        std::atomic<bool> go{false};
        std::atomic<bool> won{false};
        std::thread claimer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            won.store(gate.try_resume(physical(12000 + i), &resumed),
                      std::memory_order_release);
        });
        std::thread retire([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            authority->retire();
        });
        go.store(true, std::memory_order_release);
        claimer.join();
        retire.join();
        if (won.load(std::memory_order_acquire)) {
            assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
            assert(gate.finish_resume());
            assert(ores_native_execution_context_store_release(
                stores.executions, resumed));
        } else {
            assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
            assert(!gate.finish_resume());
        }
        gate.close();
        stores.release(link);
    }
}

static void only_claiming_gate_can_finish_execution() {
    Stores stores;
    auto task = TaskCancellation::root();
    const auto link = stores.suspend(14321);
    TaskContinuationGate winner(task, stores.executions,
                                stores.continuations, link, 11,
                                {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
    TaskContinuationGate other(task, stores.executions,
                               stores.continuations, link, 11,
                               {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
    assert(winner.notify_ready());
    OresNativeExecutionHandle admitted{};
    assert(winner.try_resume(physical(14322), &admitted));
    assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
    // The second binding cannot steal the physical completion authority.
    assert(!other.finish_resume());
    assert(!other.try_resume(physical(14323), &admitted));
    assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
    other.close();
    winner.close();
    assert(winner.finish_resume());
    assert(!winner.finish_resume());
    assert(ores_native_execution_context_store_release(
        stores.executions, admitted));
    stores.release(link);
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
                                      stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
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
                                      stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
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
                                  stores.continuations, link, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});
        std::thread cancellation([&] { (void)parent->cancel("shutdown"); });
        gate.close();
        cancellation.join();
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        stores.release(link);
        child->retire();
        parent->retire();
    }
}


static void join_continuation_requires_exact_explicit_boundary() {
    Stores stores;
    auto task = TaskCancellation::root();
    const auto join = stores.suspend(21000, ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
    const OresNativeAllocationDomainRef actor{ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4};
    bool rejected = false;
    try {
        // Existing await call sites must NOT silently gain TASK_JOIN authority.
        TaskContinuationGate gate(task, stores.executions, stores.continuations,
                                  join, 11, actor);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions, stores.continuations,
                                  join, 12, actor, ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); // code reload cannot revive stale native joins
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions, stores.continuations,
                                  join, 11, {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 9, 4},
                                  ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); // actor heap identity cannot be borrowed
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions, stores.continuations,
                                  join, 11, actor,
                                  ORES_CONTINUATION_BOUNDARY_SUPERVISOR_CONTROL);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); // no privilege escalation via expected_boundary

    {
        TaskContinuationGate gate(task, stores.executions, stores.continuations,
                                  join, 11, actor, ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
        assert(gate.notify_ready());
        assert(!gate.notify_ready());
        OresNativeExecutionHandle resumed{};
        assert(gate.try_resume(physical(21001), &resumed));
        assert(!gate.try_resume(physical(21002), &resumed));
        assert(gate.finish_resume());
        assert(!gate.finish_resume());
        assert(ores_native_execution_context_store_release(
            stores.executions, resumed));
    }
    stores.release(join);

    const auto await = stores.suspend(21010);
    rejected = false;
    try {
        TaskContinuationGate gate(task, stores.executions, stores.continuations,
                                  await, 11, actor, ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
    } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); // TASK_JOIN cannot steal a Future AWAIT record
    assert(ores_native_continuation_store_cancel(
        stores.continuations, await.continuation));
    stores.release(await);
    task->retire();
}

static void join_notification_and_task_retirement_race() {
    Stores stores;
    const OresNativeAllocationDomainRef actor{ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4};
    for (std::uint64_t i = 0; i < 64; ++i) {
        auto task = TaskCancellation::root();
        const auto link = stores.suspend(22000 + i, ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
        TaskContinuationGate gate(task, stores.executions, stores.continuations,
                                  link, 11, actor, ORES_CONTINUATION_BOUNDARY_TASK_JOIN);
        std::atomic<bool> go{false};
        std::thread completion([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)gate.notify_ready();
        });
        std::thread retirement([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            task->retire();
        });
        go.store(true, std::memory_order_release);
        completion.join();
        retirement.join();
        assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
        OresNativeExecutionHandle resumed{};
        assert(!gate.try_resume(physical(23000 + i), &resumed));
        gate.close();
        stores.release(link);
    }
}

#if defined(__linux__)
static void reactor_completion_handoff_precedes_actor_done() {
    NativeActorGroup group(990001, 1);
    auto actor = group.adopt(990001, 1, 1, [] {});
    assert(group.mark_ready(actor));
    NativeEpollReactorManager manager(1);

    int pipefd[2]{-1, -1};
    assert(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    auto watch = manager.try_watch_actor(actor, pipefd[0], EPOLLIN);
    assert(watch);
    assert(actor.outstanding_resource_loans() == 1);

    Stores stores;
    auto task = TaskCancellation::root();
    const auto link = stores.suspend(16000);
    TaskContinuationGate gate(
        task, stores.executions, stores.continuations, link, 11,
        {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});

    assert(::write(pipefd[1], "x", 1) == 1);
    std::optional<NativeEpollReactorManager::Completion> completion;
    for (int retry = 0; retry < 3000 && !completion; ++retry) {
        completion = manager.try_pop();
        if (!completion) std::this_thread::yield();
    }
    assert(completion && completion->actor
           && completion->actor->same_incarnation(actor));
    assert(completion->event.outcome == NativeEpollReactor::Outcome::ready);

    group.stop_workers();
    assert(!group.finalized(actor));

    // First acquire generation-checked continuation readiness ownership.
    assert(gate.notify_ready());
    assert(stores.state(link) == ORES_CONTINUATION_READY);
    // Only after READY escrow exists may the reactor's physical loan leave.
    assert(manager.ack_actor_completion(*completion));
    assert(!manager.ack_actor_completion(*completion));
    assert(actor.outstanding_resource_loans() == 0);
    assert(group.finalized(actor));
    actor.get_done().get();

    OresNativeExecutionHandle resumed{};
    assert(gate.try_resume(physical(16001), &resumed));
    assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
    assert(gate.finish_resume());
    assert(ores_native_execution_context_store_release(
        stores.executions, resumed));
    gate.close();
    stores.release(link);
    task->retire();

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}

static void reactor_completion_cancel_before_enqueue_releases_safely() {
    NativeActorGroup group(990002, 1);
    auto actor = group.adopt(990002, 1, 1, [] {});
    assert(group.mark_ready(actor));
    NativeEpollReactorManager manager(1);

    int pipefd[2]{-1, -1};
    assert(::pipe2(pipefd, O_NONBLOCK | O_CLOEXEC) == 0);
    auto watch = manager.try_watch_actor(actor, pipefd[0], EPOLLIN);
    assert(watch);

    Stores stores;
    auto parent = TaskCancellation::root();
    auto child = parent->child();
    const auto link = stores.suspend(17000);
    TaskContinuationGate gate(
        child, stores.executions, stores.continuations, link, 11,
        {ORES_ALLOCATION_DOMAIN_ACTOR, 88, 3, 4});

    assert(::write(pipefd[1], "x", 1) == 1);
    std::optional<NativeEpollReactorManager::Completion> completion;
    for (int retry = 0; retry < 3000 && !completion; ++retry) {
        completion = manager.try_pop();
        if (!completion) std::this_thread::yield();
    }
    assert(completion && completion->actor
           && completion->actor->same_incarnation(actor));

    // Completion exists, but cancellation wins before the scheduler publishes
    // READY. The continuation is irrevocably cancelled, so no guest resume can
    // own this terminal record; releasing the reactor loan is now safe.
    assert(parent->cancel("nio-cancel-after-completion"));
    assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
    assert(!gate.notify_ready());

    group.stop_workers();
    assert(!group.finalized(actor));
    assert(manager.ack_actor_completion(*completion));
    assert(actor.outstanding_resource_loans() == 0);
    assert(group.finalized(actor));
    actor.get_done().get();

    OresNativeExecutionHandle resumed{};
    assert(!gate.try_resume(physical(17001), &resumed));
    gate.close();
    stores.release(link);
    child->retire();
    parent->retire();

    ::close(pipefd[0]);
    ::close(pipefd[1]);
}
#endif

int main() {
    revokes_pending_and_drains_children();
    late_task_cancel_cannot_reenter_claimed_guest();
    already_cancelled_authority_closes_registration_race();
    rejects_forged_and_recycled_await_links();
    rejects_non_await_control_and_spawn_boundaries();
    retirement_is_a_strict_waiter_revocation_barrier();
    retirement_and_ready_resume_claim_are_linearized();
    only_claiming_gate_can_finish_execution();
    completion_cancel_and_resume_cancel_races();
    close_disarms_concurrent_revocation();
    join_continuation_requires_exact_explicit_boundary();
    join_notification_and_task_retirement_race();
#if defined(__linux__)
    reactor_completion_handoff_precedes_actor_done();
    reactor_completion_cancel_before_enqueue_releases_safely();
#endif
    std::puts("NATIVE_TASK_CONTINUATION_GATE_PASS");
}
