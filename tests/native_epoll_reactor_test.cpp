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
    auto cancelled = reactor.try_watch(accepted, EPOLLIN);
    expect(cancelled.has_value(), "register cancel target");
    expect(reactor.try_cancel(*cancelled), "cancel should win");
    expect(!reactor.try_cancel(*cancelled), "duplicate cancel rejected");
    auto cancellation = until_completion(reactor);
    expect(cancellation.has_value() && cancellation->outcome == NativeEpollReactor::Outcome::cancelled, "cancel record");
    auto replacement = reactor.try_watch(accepted, EPOLLIN);
    expect(replacement.has_value() && replacement->value != cancelled->value, "new generation");
    expect(!reactor.try_cancel(*cancelled), "old ticket must not cancel new operation");
    reactor.stop();
    auto stopped = until_completion(reactor);
    expect(stopped.has_value() && stopped->ticket.value == replacement->value
           && stopped->outcome == NativeEpollReactor::Outcome::stopped, "stop completion");
    expect(reactor.pending() == 0, "fully drained");
    expect(!reactor.try_watch(accepted, EPOLLIN).has_value(), "admission fenced after shutdown");
    ::close(accepted); ::close(client); ::close(server);
    return 0;
}
