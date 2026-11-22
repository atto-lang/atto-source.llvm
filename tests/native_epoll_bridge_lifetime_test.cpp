#include "oreslang/runtime/NativeEpollTransportBridge.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <memory>
#include <optional>
#include <thread>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace oreslang::runtime;
using Bridge = NativeEpollTransportBridge;

static std::optional<Bridge::Ticket> watch(Bridge& bridge, int fd) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        auto ticket = bridge.try_watch(fd, EPOLLIN);
        if (ticket) return ticket;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}
static std::optional<Bridge::CompletionLease> pop(Bridge& bridge) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        auto completion = bridge.try_pop();
        if (completion) return completion;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}
static void test_clean_close() {
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0, fds) == 0);
    NativeTransportCallbackGate gate(7, 8, 9, 10);
    {
        Bridge bridge(gate);
        assert(bridge.start());
        auto ticket = watch(bridge, fds[0]);
        assert(ticket);
        bridge.stop();
        {
            auto completion = pop(bridge);
            assert(completion && completion->permit);
            assert(completion->event.ticket.value == ticket->value);
            assert(!gate.try_drain());
        }
        assert(gate.try_drain());
    }
    assert(::close(fds[0]) == 0);
    assert(::close(fds[1]) == 0);
}
static void child_must_abort_on_handoff() {
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0, fds) == 0);
    NativeTransportCallbackGate gate(7, 8, 9, 11);
    auto bridge = std::make_unique<Bridge>(gate);
    assert(bridge->start());
    auto ticket = watch(*bridge, fds[0]);
    assert(ticket);
    bridge->stop();
    auto completion = pop(*bridge);
    assert(completion && completion->permit);
    assert(completion->event.ticket.value == ticket->value);
    assert(!gate.try_drain());
    // The old destructor checked only its now-empty slots and returned,
    // allowing an outstanding completion to outlive the bridge. An actor
    // teardown MUST fail-stop instead of weakening callback provenance.
    bridge.reset();  // Expected SIGABRT, before gate/completion destruction.
    _exit(77);       // A normal exit indicates a regression.
}
int main() {
    test_clean_close();
    const pid_t pid = ::fork();
    assert(pid >= 0);
    if (pid == 0) {
        child_must_abort_on_handoff();
    }
    int status = 0;
    assert(::waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    std::puts("NATIVE_EPOLL_BRIDGE_LIFETIME_FAILSTOP_OK");
}
