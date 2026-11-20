#include "oreslang/runtime/AsyncIoEpollOperation.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <sys/socket.h>

using namespace oreslang::runtime;

static void successful_readiness_path() {
    int pair[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    EpollReadiness readiness;
    const AsyncIoTicket ticket{21, 4, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 41);
    operation.arm();

    const char payload[] = "ok";
    const auto sent = try_write(writer.get(), payload, sizeof(payload) - 1);
    assert(sent.disposition == IoDisposition::transferred);

    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    assert(readiness.wait(ready, 1000) == 1);
    assert(operation.claim_ready(ready[0]));

    std::array<char, 8> buffer{};
    const auto read = try_read(reader.get(), buffer.data(), buffer.size());
    assert(read.disposition == IoDisposition::transferred);
    assert(read.size == sizeof(payload) - 1);
    assert(operation.finish_callback(AsyncIoTerminal::success));

    // Logical completion alone is not reclaimable while epoll still owns fd.
    assert(!lifecycle.ready_to_reclaim());
    operation.disarm();
    assert(operation.retire());
    assert(lifecycle.ready_to_reclaim());
}

static void would_block_rearm_path() {
    int pair[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    EpollReadiness readiness;
    const AsyncIoTicket ticket{22, 5, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 42);
    operation.arm();

    const char first = 'a';
    assert(try_write(writer.get(), &first, 1).disposition == IoDisposition::transferred);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    assert(readiness.wait(ready, 1000) == 1);
    assert(operation.claim_ready(ready[0]));

    // Another consumer wins the byte before this callback reads it.
    std::array<char, 1> tmp{};
    assert(try_read(reader.get(), tmp.data(), tmp.size()).disposition == IoDisposition::transferred);
    assert(try_read(reader.get(), tmp.data(), tmp.size()).disposition == IoDisposition::would_block);
    assert(operation.rearm_after_would_block());

    const char second = 'b';
    assert(try_write(writer.get(), &second, 1).disposition == IoDisposition::transferred);
    assert(readiness.wait(ready, 1000) == 1);
    assert(operation.claim_ready(ready[0]));
    assert(try_read(reader.get(), tmp.data(), tmp.size()).disposition == IoDisposition::transferred);
    assert(operation.finish_callback(AsyncIoTerminal::success));
    operation.disarm();
    assert(operation.retire());
}

static void cancellation_before_readiness() {
    int pair[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    EpollReadiness readiness;
    const AsyncIoTicket ticket{23, 6, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 43);
    operation.arm();

    assert(operation.cancel());
    assert(lifecycle.terminal() == AsyncIoTerminal::cancelled);
    assert(!operation.armed());
    assert(operation.retire());
    assert(lifecycle.ready_to_reclaim());

    const char byte = 'x';
    assert(try_write(writer.get(), &byte, 1).disposition == IoDisposition::transferred);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    assert(readiness.wait(ready, 0) == 0);
}

int main() {
    successful_readiness_path();
    would_block_rearm_path();
    cancellation_before_readiness();
    return 0;
}
