#include "oreslang/runtime/AsyncIoEpollOperation.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <thread>
#include <sys/socket.h>

using namespace oreslang::runtime;

static void check(bool condition) { if (!condition) std::abort(); }

static void successful_readiness_path() {
    int pair[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    EpollReadiness readiness;
    const AsyncIoTicket ticket{21, 4, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 41);
    operation.arm();

    const char payload[] = "ok";
    const auto sent = try_write(writer.get(), payload, sizeof(payload) - 1);
    check(sent.disposition == IoDisposition::transferred);

    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    check(readiness.wait(ready, 1000) == 1);
    check(operation.claim_ready(ready[0]));

    std::array<char, 8> buffer{};
    const auto read = try_read(reader.get(), buffer.data(), buffer.size());
    check(read.disposition == IoDisposition::transferred);
    check(read.size == sizeof(payload) - 1);
    check(operation.finish_callback(AsyncIoTerminal::success));

    // Logical completion alone is not reclaimable while epoll still owns fd.
    check(!lifecycle.ready_to_reclaim());
    operation.disarm();
    check(operation.retire());
    check(lifecycle.ready_to_reclaim());
}

static void would_block_rearm_path() {
    int pair[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    EpollReadiness readiness;
    const AsyncIoTicket ticket{22, 5, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 42);
    operation.arm();

    const char first = 'a';
    check(try_write(writer.get(), &first, 1).disposition == IoDisposition::transferred);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    check(readiness.wait(ready, 1000) == 1);
    check(operation.claim_ready(ready[0]));

    // Another consumer wins the byte before this callback reads it.
    std::array<char, 1> tmp{};
    check(try_read(reader.get(), tmp.data(), tmp.size()).disposition == IoDisposition::transferred);
    check(try_read(reader.get(), tmp.data(), tmp.size()).disposition == IoDisposition::would_block);
    check(operation.rearm_after_would_block());

    const char second = 'b';
    check(try_write(writer.get(), &second, 1).disposition == IoDisposition::transferred);
    check(readiness.wait(ready, 1000) == 1);
    check(operation.claim_ready(ready[0]));
    check(try_read(reader.get(), tmp.data(), tmp.size()).disposition == IoDisposition::transferred);
    check(operation.finish_callback(AsyncIoTerminal::success));
    operation.disarm();
    check(operation.retire());
}

static void cancellation_before_readiness() {
    int pair[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]);
    OwnedSocketFd writer(pair[1]);

    EpollReadiness readiness;
    const AsyncIoTicket ticket{23, 6, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 43);
    operation.arm();

    check(operation.cancel());
    check(lifecycle.terminal() == AsyncIoTerminal::cancelled);
    check(!operation.armed());
    check(operation.retire());
    check(lifecycle.ready_to_reclaim());

    const char byte = 'x';
    check(try_write(writer.get(), &byte, 1).disposition == IoDisposition::transferred);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    check(readiness.wait(ready, 0) == 0);
}

static void cancelled_would_block_releases_callback_credit() {
    for (unsigned mode = 0; mode < 2; ++mode) {
        int pair[2] = {-1, -1};
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        OwnedSocketFd reader(pair[0]), writer(pair[1]);
        EpollReadiness readiness;
        const AsyncIoTicket ticket{24, 7, mode + 1};
        AsyncIoOperationLifecycle lifecycle(ticket);
        AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 44);
        operation.arm();
        check(!operation.finish_callback(AsyncIoTerminal::success));
        check(lifecycle.terminal() == AsyncIoTerminal::pending);
        const char byte = 'x';
        check(try_write(writer.get(), &byte, 1).disposition == IoDisposition::transferred);
        std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
        check(readiness.wait(ready, 1000) == 1);
        check(operation.claim_ready(ready[0]));
        check(!operation.finish_callback(static_cast<AsyncIoTerminal>(255)));
        check(lifecycle.callback_inflight());
        char received{};
        check(try_read(reader.get(), &received, 1).disposition == IoDisposition::transferred);
        check(try_read(reader.get(), &received, 1).disposition == IoDisposition::would_block);
        check(mode == 0 ? operation.cancel() : operation.timeout());
        check(!operation.retire());
        check(!operation.rearm_after_would_block());
        check(!lifecycle.callback_inflight());
        check(!operation.armed());
        check(operation.retire());
        check(lifecycle.ready_to_reclaim());
        check(!operation.rearm_after_would_block());
        check(!operation.claim_ready(ready[0]));
    }
}

static void cancellation_racing_rearm_never_strands_credit() {
    for (unsigned i = 0; i < 256; ++i) {
        int pair[2] = {-1, -1};
        check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
        OwnedSocketFd reader(pair[0]), writer(pair[1]);
        EpollReadiness readiness;
        const AsyncIoTicket ticket{25, 8, i + 1};
        AsyncIoOperationLifecycle lifecycle(ticket);
        AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 45);
        operation.arm();
        const char byte = 'x';
        check(try_write(writer.get(), &byte, 1).disposition == IoDisposition::transferred);
        std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
        check(readiness.wait(ready, 1000) == 1);
        check(operation.claim_ready(ready[0]));
        char received{};
        check(try_read(reader.get(), &received, 1).disposition == IoDisposition::transferred);
        check(try_read(reader.get(), &received, 1).disposition == IoDisposition::would_block);
        std::thread canceller([&] { check(operation.cancel()); });
        (void)operation.rearm_after_would_block();
        canceller.join();
        check(!operation.armed() && !lifecycle.callback_inflight());
        check(operation.retire() && lifecycle.ready_to_reclaim());
        check(!operation.claim_ready(ready[0]));
    }
}

static void foreign_lifecycle_ticket_rejected_before_registration() {
    int pair[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]), writer(pair[1]);
    EpollReadiness readiness;
    AsyncIoOperationLifecycle lifecycle({26, 9, 1});
    bool rejected = false;
    try {
        AsyncIoEpollOperation foreign(readiness, lifecycle, {26, 10, 1}, reader.get(), EPOLLIN, 46);
    } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && !lifecycle.registered());
}

// Fault injection deliberately violates the ordinary retained-fd precondition.
// A duplicate keeps the original open-file description alive while DEL on
// the closed numeric fd returns EBADF. Restore exactly the same identity
// before retry; never use this sequence as a production recovery protocol.
static void failed_kernel_delete_does_not_refund_registration_credit() {
    int pair[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]), writer(pair[1]);
    const int original = reader.get();
    OwnedSocketFd duplicate(::dup(original));
    check(duplicate.valid());
    EpollReadiness readiness;
    const AsyncIoTicket ticket{27, 9, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, original, EPOLLIN, 47);
    operation.arm();
    reader.reset();
    check(operation.cancel());
    check(operation.armed() && lifecycle.registered());
    check(!operation.retire() && !lifecycle.ready_to_reclaim());
    const char byte = 'q';
    check(try_write(writer.get(), &byte, 1).disposition == IoDisposition::transferred);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    check(readiness.wait(ready, 1000) == 1 && ready[0].token == 47);
    check(!operation.claim_ready(ready[0]));
    check(::dup3(duplicate.get(), original, O_CLOEXEC) == original);
    reader = OwnedSocketFd(original);
    operation.disarm();
    check(!operation.armed() && !lifecycle.registered());
    check(operation.retire() && lifecycle.ready_to_reclaim());
    check(!operation.claim_ready(ready[0]));
}

static void already_removed_retained_descriptor_can_retire() {
    int pair[2] = {-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair) == 0);
    OwnedSocketFd reader(pair[0]), writer(pair[1]);
    EpollReadiness readiness;
    const AsyncIoTicket ticket{28, 9, 1};
    AsyncIoOperationLifecycle lifecycle(ticket);
    AsyncIoEpollOperation operation(readiness, lifecycle, ticket, reader.get(), EPOLLIN, 48);
    operation.arm();
    check(readiness.remove_confirmed(reader.get()));
    check(operation.cancel());
    check(!operation.armed() && operation.retire());
    check(lifecycle.ready_to_reclaim());
}

int main() {
    failed_kernel_delete_does_not_refund_registration_credit();
    already_removed_retained_descriptor_can_retire();
    cancelled_would_block_releases_callback_credit();
    cancellation_racing_rearm_never_strands_credit();
    foreign_lifecycle_ticket_rejected_before_registration();
    successful_readiness_path();
    would_block_rearm_path();
    cancellation_before_readiness();
    return 0;
}
