#include "oreslang/runtime/AsyncIoContinuationGate.hpp"
#include "oreslang/runtime/AsyncIoEpollOperation.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <sys/socket.h>

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
            const AsyncIoTicket& ticket) {
        OresNativeExecutionContext context{};
        context.id.runtime_domain = 91;
        context.id.execution = execution_id;
        context.allocation.kind = ORES_ALLOCATION_DOMAIN_ACTOR;
        context.allocation.domain = 91;
        context.allocation.owner = ticket.actor_id;
        context.allocation.generation = ticket.generation;

        OresNativeExecutionHandle origin{};
        assert(ores_native_execution_context_store_insert(
            executions, &context, &origin));

        OresNativeSuspensionLink link{};
        assert(ores_native_await_trace_suspend(
            executions, continuations, origin,
            OresNativeContinuationHandle{},
            ORES_CONTINUATION_BOUNDARY_AWAIT,
            23, ticket.operation_id, &link));
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

static void real_epoll_completion_resumes_only_after_retirement() {
    int pair[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    Stores stores;
    const AsyncIoTicket ticket{31, 8, 701};
    const auto link = stores.suspend(7001, ticket);

    EpollReadiness readiness;
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation epoll(
        readiness, lifecycle, ticket, reader.get(), EPOLLIN, 501);
    AsyncIoContinuationGate continuation(
        lifecycle, stores.executions, stores.continuations, link, 23);

    epoll.arm();
    const char byte = 'q';
    assert(try_write(writer.get(), &byte, 1).disposition == IoDisposition::transferred);

    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    assert(readiness.wait(ready, 1000) == 1);
    assert(epoll.claim_ready(ready[0]));

    char received = 0;
    assert(try_read(reader.get(), &received, 1).disposition == IoDisposition::transferred);
    assert(received == byte);
    assert(epoll.finish_callback(AsyncIoTerminal::success));

    // Completion is not scheduler-visible while epoll still owns registration.
    assert(!continuation.notify_ready_after_retirement());
    assert(stores.state(link) == ORES_CONTINUATION_PENDING);

    epoll.disarm();
    assert(epoll.retire());
    assert(lifecycle.ready_to_reclaim());
    assert(continuation.notify_ready_after_retirement());
    assert(stores.state(link) == ORES_CONTINUATION_READY);

    OresNativeExecutionHandle resumed{};
    assert(continuation.try_resume({91, 7002}, &resumed));
    assert(stores.state(link) == ORES_CONTINUATION_CLAIMED);
    assert(continuation.finish_resume());
    assert(ores_native_execution_context_store_release(stores.executions, resumed));
    stores.release(link);
}

static void cancellation_after_ready_before_resume_wins_without_reentry() {
    int pair[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    Stores stores;
    const AsyncIoTicket ticket{32, 9, 702};
    const auto link = stores.suspend(7101, ticket);

    EpollReadiness readiness;
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation epoll(
        readiness, lifecycle, ticket, reader.get(), EPOLLIN, 502);
    AsyncIoContinuationGate continuation(
        lifecycle, stores.executions, stores.continuations, link, 23);

    epoll.arm();
    const char byte = 'z';
    assert(try_write(writer.get(), &byte, 1).disposition == IoDisposition::transferred);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    assert(readiness.wait(ready, 1000) == 1);
    assert(epoll.claim_ready(ready[0]));

    char received = 0;
    assert(try_read(reader.get(), &received, 1).disposition == IoDisposition::transferred);
    assert(epoll.finish_callback(AsyncIoTerminal::success));
    epoll.disarm();
    assert(epoll.retire());
    assert(continuation.notify_ready_after_retirement());
    assert(stores.state(link) == ORES_CONTINUATION_READY);

    // Continuation cancellation is allowed to revoke READY before the scheduler
    // claims it. The physically retired I/O operation must not re-enter guest.
    assert(continuation.cancel());
    assert(stores.state(link) == ORES_CONTINUATION_CANCELLED);

    OresNativeExecutionHandle resumed{};
    assert(!continuation.try_resume({91, 7102}, &resumed));
    stores.release(link);
}

int main() {
    real_epoll_completion_resumes_only_after_retirement();
    cancellation_after_ready_before_resume_wins_without_reentry();
    return 0;
}
