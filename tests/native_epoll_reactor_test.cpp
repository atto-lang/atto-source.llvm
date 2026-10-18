#include "oreslang/runtime/NativeEpollReactor.hpp"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
using namespace oreslang::runtime;

static void expect(bool x, const char* message) { if (!x) throw std::runtime_error(message); }
static std::optional<NativeEpollReactor::Completion> until_completion(NativeEpollReactor& reactor) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        auto completion = reactor.try_pop();
        if (completion) return completion;
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // test-only
    }
    return std::nullopt;
}
int main() {
    NativeEpollReactor reactor;
    expect(reactor.pending() == 0, "cold pending must be zero");
    expect(reactor.start(), "start");
    expect(reactor.start(), "repeated start");
    expect(!reactor.try_watch(-1, EPOLLIN).has_value(), "bad fd");
    expect(!reactor.try_watch(0, 0).has_value(), "bad readiness flags");
    // A readiness notification alone must never legalize blocking I/O.
    // Reject a blocking descriptor; allow that same descriptor only after
    // its owner explicitly enables O_NONBLOCK before guest admission.
    int blocking_pipe[2]{-1, -1};
    expect(::pipe(blocking_pipe) == 0, "create blocking pipe");
    expect(!reactor.try_watch(blocking_pipe[0], EPOLLIN).has_value(),
           "blocking descriptor must fail closed");
    const int old_flags = ::fcntl(blocking_pipe[0], F_GETFL);
    expect(old_flags >= 0
           && ::fcntl(blocking_pipe[0], F_SETFL, old_flags | O_NONBLOCK) == 0,
           "set nonblocking pipe");
    auto nonblocking_ticket = reactor.try_watch(blocking_pipe[0], EPOLLIN);
    expect(nonblocking_ticket.has_value(), "nonblocking descriptor permitted");
    expect(reactor.try_cancel(*nonblocking_ticket), "cancel test pipe watch");
    auto cancelled_pipe = until_completion(reactor);
    expect(cancelled_pipe.has_value()
           && cancelled_pipe->ticket.value == nonblocking_ticket->value
           && cancelled_pipe->outcome == NativeEpollReactor::Outcome::cancelled,
           "cancel test pipe");
    ::close(blocking_pipe[0]);
    ::close(blocking_pipe[1]);
    const int server = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    expect(server >= 0, "socket");
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    expect(::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
    expect(::listen(server, 32) == 0, "listen");
    socklen_t addrlen = sizeof(addr);
    expect(::getsockname(server, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0, "getsockname");
    auto accept_ticket = reactor.try_watch(server, EPOLLIN);
    expect(accept_ticket.has_value(), "register listener");
    expect(reactor.pending() == 1, "pending listener");
    const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    expect(client >= 0, "client socket");
    const int result = ::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    expect(result == 0 || (result == -1 && errno == EINPROGRESS), "nonblocking connect");
    auto accepted_event = until_completion(reactor);
    expect(accepted_event.has_value(), "accept readiness timeout");
    expect(accepted_event->ticket.value == accept_ticket->value, "ticket identity");
    expect((accepted_event->events & EPOLLIN) != 0, "accept readiness event");
    expect(reactor.pending() == 0, "slot released at consumption");
    const int accepted = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    expect(accepted >= 0, "accepted connection");
    auto recv_ticket = reactor.try_watch(accepted, EPOLLIN);
    expect(recv_ticket.has_value(), "readiness watch");
    expect(::send(client, "hello", 5, MSG_DONTWAIT | MSG_NOSIGNAL) == 5, "send");
    auto incoming = until_completion(reactor);
    expect(incoming.has_value() && incoming->ticket.value == recv_ticket->value, "readiness result");
    std::array<char, 8> bytes{};
    expect(::recv(accepted, bytes.data(), bytes.size(), MSG_DONTWAIT) == 5, "read bytes");
    expect(std::memcmp(bytes.data(), "hello", 5) == 0, "payload");
    // One-shot epoll readiness is re-armed only after a terminal ticket
    // has been consumed. Exercise the same-socket kernel-read path many
    // times, not just the cancellation/re-registration path below.
    for (int round = 0; round < 64; ++round) {
        auto repeated = reactor.try_watch(accepted, EPOLLIN);
        expect(repeated.has_value(), "repeat readiness registration");
        const char byte = static_cast<char>(round);
        expect(::send(client, &byte, 1, MSG_DONTWAIT | MSG_NOSIGNAL) == 1,
               "repeat send");
        auto ready = until_completion(reactor);
        expect(ready.has_value()
               && ready->ticket.value == repeated->value
               && ready->outcome == NativeEpollReactor::Outcome::ready,
               "repeat readiness ticket and outcome");
        char received = 0;
        expect(::recv(accepted, &received, 1, MSG_DONTWAIT) == 1
               && received == byte, "repeat nonblocking receive");
        expect(reactor.pending() == 0, "one-shot readiness slot released");
    }
    auto cancelled = reactor.try_watch(accepted, EPOLLIN);
    expect(cancelled.has_value(), "register cancel target");
    expect(reactor.try_cancel(*cancelled), "cancel should win");
    expect(!reactor.try_cancel(*cancelled), "duplicate cancel rejected");
    auto cancellation = until_completion(reactor);
    expect(cancellation.has_value() && cancellation->outcome == NativeEpollReactor::Outcome::cancelled, "cancel record");
    // Java Selector requires an explicit cancelled-key sweep before
    // re-registration. Linux epoll uses synchronous EPOLL_CTL_DEL instead:
    // prove the equivalent one-owner re-arm contract for many generations.
    auto previous_ticket = *cancelled;
    for (int round = 0; round < 128; ++round) {
        auto watched_again = reactor.try_watch(accepted, EPOLLIN);
        expect(watched_again.has_value(), "re-arm after cancel and pop");
        expect(watched_again->value != previous_ticket.value, "fresh ticket generation");
        expect(!reactor.try_cancel(previous_ticket),
               "retired ticket cannot cancel a newly admitted watch");
        expect(reactor.try_cancel(*watched_again), "cancel current generation");
        auto terminal = until_completion(reactor);
        expect(terminal.has_value()
               && terminal->ticket.value == watched_again->value
               && terminal->outcome == NativeEpollReactor::Outcome::cancelled,
               "one terminal record per cancelled generation");
        expect(reactor.pending() == 0, "re-arm must release slot on pop");
        previous_ticket = *watched_again;
    }
    auto replacement = reactor.try_watch(accepted, EPOLLIN);
    expect(replacement.has_value() && replacement->value != previous_ticket.value, "new generation");
    expect(!reactor.try_cancel(*cancelled), "old ticket must not cancel new operation");
    reactor.stop();
    expect(!reactor.try_cancel(*replacement),
           "shutdown fence must reject late cancellation of stopped ticket");
    auto stopped = until_completion(reactor);
    expect(stopped.has_value() && stopped->ticket.value == replacement->value
           && stopped->outcome == NativeEpollReactor::Outcome::stopped, "stop completion");
    expect(reactor.pending() == 0, "fully drained");
    expect(!reactor.try_watch(accepted, EPOLLIN).has_value(), "admission fenced after shutdown");
    reactor.stop(); // idempotent host teardown must preserve terminal state
    ::close(accepted); ::close(client); ::close(server);

    // Race two independent host teardown callers against one outstanding
    // readiness ticket. The std::thread join handle has exactly one owner,
    // and a stopped completion must not be duplicated by the second caller.
    for (int round = 0; round < 24; ++round) {
        NativeEpollReactor competing;
        expect(competing.start(), "start concurrent-stop reactor");
        int ends[2]{-1, -1};
        expect(::pipe(ends) == 0, "create concurrent-stop pipe");
        const int flags = ::fcntl(ends[0], F_GETFL);
        expect(flags >= 0
               && ::fcntl(ends[0], F_SETFL, flags | O_NONBLOCK) == 0,
               "make concurrent-stop pipe nonblocking");
        const auto ticket = competing.try_watch(ends[0], EPOLLIN);
        expect(ticket.has_value(), "register stop-race ticket");

        std::thread first([&] { competing.stop(); });
        std::thread second([&] { competing.stop(); });
        first.join();
        second.join();

        auto completion = until_completion(competing);
        expect(completion.has_value()
               && completion->ticket.value == ticket->value
               && completion->outcome == NativeEpollReactor::Outcome::stopped,
               "exactly one stopped completion after competing teardown");
        expect(!competing.try_pop().has_value(),
               "competing stop must not emit duplicate terminal completion");
        expect(competing.pending() == 0, "stop-race completion must reclaim ticket");
        expect(!competing.try_cancel(*ticket), "stop-race ticket cannot cancel after teardown");
        expect(!competing.try_watch(ends[0], EPOLLIN).has_value(),
               "stop-race reactor must reject late watch");
        ::close(ends[0]);
        ::close(ends[1]);
    }
    return 0;
}
