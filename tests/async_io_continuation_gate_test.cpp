#include "oreslang/runtime/AsyncIoContinuationGate.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>

using namespace oreslang::runtime;

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

    OresNativeSuspensionLink suspend(
            std::uint64_t execution_id,
            std::uint64_t actor_id,
            std::uint64_t actor_generation,
            std::uint64_t operation_id) {
        OresNativeExecutionContext context{};
        context.id.runtime_domain = 77;
        context.id.execution = execution_id;
        context.allocation.kind = ORES_ALLOCATION_DOMAIN_ACTOR;
        context.allocation.domain = 77;
        context.allocation.owner = actor_id;
        context.allocation.generation = actor_generation;

        OresNativeExecutionHandle origin{};
        assert(ores_native_execution_context_store_insert(
            executions, &context, &origin));

        OresNativeSuspensionLink link{};
        assert(ores_native_await_trace_suspend(
            executions, continuations, origin,
            OresNativeContinuationHandle{},
            ORES_CONTINUATION_BOUNDARY_AWAIT,
            17, operation_id, &link));
        assert(ores_native_execution_context_store_release(executions, origin));
        return link;
    }

    OresNativeContinuationState state(OresNativeSuspensionLink link) {
        OresNativeContinuationRecord record{};
        assert(ores_native_continuation_store_get(
            continuations, link.continuation, &record));
        return record.state;
    }

    void release(OresNativeSuspensionLink link) {
        assert(ores_native_continuation_store_release(
            continuations, link.continuation));
    }
};

static void completion_is_not_scheduler_visible_before_physical_retirement() {
    Stores stores;
    const AsyncIoTicket ticket{9, 4, 101};
    auto link = stores.suspend(1001, ticket.actor_id, ticket.generation,
                               ticket.operation_id);

    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoContinuationGate gate(
        lifecycle, stores.executions, stores.continuations, link, 17);

    assert(lifecycle.mark_registered(ticket));
    assert(lifecycle.begin_callback(ticket));
    assert(lifecycle.finish(ticket, AsyncIoTerminal::success));
    assert(lifecycle.end_callback(ticket));

    // Logical terminal exists, but the kernel registration is still owned.
    assert(!gate.notify_ready_after_retirement());
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    assert(lifecycle.mark_unregistered(ticket));
    assert(!gate.notify_ready_after_retirement()); // frame not retired yet
    assert(lifecycle.retire(ticket));
    assert(lifecycle.ready_to_reclaim());

    assert(gate.notify_ready_after_retirement());
    assert(stores.state(link) == ORES_CONTINUATION_READY);

    OresNativeExecutionHandle resumed{};
    OresNativeExecutionContext resumed_context{};
    assert(gate.try_resume({77, 1002}, &resumed, &resumed_context));
    assert(resumed_context.allocation.owner == ticket.actor_id);
    assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
    assert(gate.finish_resume());
    assert(ores_native_execution_context_store_release(
        stores.executions, resumed));
    stores.release(link);
}

static void cancellation_revokes_waiter_but_still_requires_native_retirement() {
    Stores stores;
    const AsyncIoTicket ticket{10, 5, 202};
    auto link = stores.suspend(2001, ticket.actor_id, ticket.generation,
                               ticket.operation_id);

    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoContinuationGate gate(
        lifecycle, stores.executions, stores.continuations, link, 17);

    assert(lifecycle.mark_registered(ticket));
    assert(gate.cancel());
    assert(lifecycle.terminal() == AsyncIoTerminal::cancelled);
    assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);
    assert(!lifecycle.ready_to_reclaim());

    assert(lifecycle.mark_unregistered(ticket));
    assert(lifecycle.retire(ticket));
    assert(lifecycle.ready_to_reclaim());
    assert(!gate.notify_ready_after_retirement());

    OresNativeExecutionHandle resumed{};
    assert(!gate.try_resume({77, 2002}, &resumed));
    stores.release(link);
}

static void stale_generation_and_operation_identity_are_rejected() {
    Stores stores;
    const AsyncIoTicket ticket{11, 6, 303};
    auto link = stores.suspend(3001, ticket.actor_id, ticket.generation,
                               ticket.operation_id);

    for (const AsyncIoTicket forged : {
            AsyncIoTicket{ticket.actor_id, ticket.generation + 1, ticket.operation_id},
            AsyncIoTicket{ticket.actor_id, ticket.generation, ticket.operation_id + 1},
            AsyncIoTicket{ticket.actor_id + 1, ticket.generation, ticket.operation_id}}) {
        AsyncIoOperationLifecycle lifecycle(forged);
        bool rejected = false;
        try {
            AsyncIoContinuationGate gate(
                lifecycle, stores.executions, stores.continuations, link, 17);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }

    assert(ores_native_continuation_store_cancel(
        stores.continuations, link.continuation));
    stores.release(link);
}

static void cancellation_and_ready_publication_are_linearized_by_store_state() {
    Stores stores;
    for (std::uint64_t i = 0; i < 256; ++i) {
        const AsyncIoTicket ticket{12, 7, 1000 + i};
        auto link = stores.suspend(4000 + i, ticket.actor_id, ticket.generation,
                                   ticket.operation_id);
        AsyncIoOperationLifecycle lifecycle(ticket);
        AsyncIoContinuationGate gate(
            lifecycle, stores.executions, stores.continuations, link, 17);

        assert(lifecycle.finish(ticket, AsyncIoTerminal::success));
        assert(lifecycle.retire(ticket));

        std::atomic<bool> go{false};
        std::atomic<bool> ready{false};
        std::atomic<bool> cancelled{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            ready.store(gate.notify_ready_after_retirement(),
                        std::memory_order_release);
        });
        std::thread canceller([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            cancelled.store(gate.cancel(), std::memory_order_release);
        });
        go.store(true, std::memory_order_release);
        producer.join();
        canceller.join();

        const auto state = stores.state(link);
        assert(state == ORES_CONTINUATION_READY
            || state == ORES_CONTINUATION_CANCELLED);
        if (state == ORES_CONTINUATION_CANCELLED) {
            OresNativeExecutionHandle resumed{};
            assert(!gate.try_resume({77, 9000 + i}, &resumed));
        }
        (void)ready;
        (void)cancelled;
        stores.release(link);
    }
}

int main() {
    completion_is_not_scheduler_visible_before_physical_retirement();
    cancellation_revokes_waiter_but_still_requires_native_retirement();
    stale_generation_and_operation_identity_are_rejected();
    cancellation_and_ready_publication_are_linearized_by_store_state();
    return 0;
}
