#include "oreslang/runtime/NativeEpollTransportBridge.hpp"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <thread>
#include <type_traits>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>

using oreslang::runtime::NativeEpollTransportBridge;
using oreslang::runtime::NativeTransportCallbackGate;
static_assert(!std::is_copy_constructible_v<NativeEpollTransportBridge::CompletionLease>);

static std::optional<NativeEpollTransportBridge::CompletionLease>
await_completion(NativeEpollTransportBridge& bridge) {
    for (int i = 0; i < 1000; ++i) {
        auto completion = bridge.try_pop();
        if (completion) return completion;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

int main() {
    // Real nonblocking Linux socketpair and epoll readiness. An outstanding
    // watch permit and then the popped envelope each prevent arena retirement.
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0, fds) == 0);
    NativeTransportCallbackGate gate(7, 3, 11, 91);
    {
        NativeEpollTransportBridge bridge(gate);
        assert(bridge.start());
        assert(!bridge.try_watch(-1, EPOLLIN));
        auto watch = bridge.try_watch(fds[0], EPOLLIN);
        assert(watch && watch->value != 0);
        assert(!bridge.try_watch(fds[0], EPOLLIN)); // already owned by reactor
        const char data = 'x';
        assert(::write(fds[1], &data, 1) == 1);
        bridge.stop();
        assert(!bridge.try_watch(fds[0], EPOLLIN));
        assert(!gate.try_drain()); // still tracked in the completion queue
        {
            auto completed = await_completion(bridge);
            assert(completed);
            assert(completed->event.ticket.value == watch->value);
            assert(completed->event.fd == fds[0]);
            assert(completed->event.outcome ==
                    NativeEpollTransportBridge::Reactor::Outcome::ready ||
                   completed->event.outcome ==
                    NativeEpollTransportBridge::Reactor::Outcome::stopped);
            assert(completed->permit);
            assert(!gate.try_drain()); // moved envelope still owns arena
        }
        const auto receipt = gate.try_drain();
        assert(receipt && gate.verifies(*receipt));
    }
    (void)::close(fds[0]);
    (void)::close(fds[1]);

    // Cancellation wins only once and publishes an owned terminal record;
    // the permit stays live through both queueing and consumer processing.
    int cancelled[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0, cancelled) == 0);
    NativeTransportCallbackGate cancellation_gate(7, 3, 12, 92);
    {
        NativeEpollTransportBridge bridge(cancellation_gate);
        assert(bridge.start());
        auto ticket = bridge.try_watch(cancelled[0], EPOLLIN);
        assert(ticket);
        assert(bridge.try_cancel(*ticket));
        assert(!bridge.try_cancel(*ticket));
        bridge.stop();
        assert(!cancellation_gate.try_drain());
        {
            auto completed = await_completion(bridge);
            assert(completed && completed->permit);
            assert(completed->event.ticket.value == ticket->value);
            assert(completed->event.outcome ==
                NativeEpollTransportBridge::Reactor::Outcome::cancelled);
            assert(!cancellation_gate.try_drain());
        }
        assert(cancellation_gate.try_drain());
    }
    (void)::close(cancelled[0]);
    (void)::close(cancelled[1]);

    std::puts("NATIVE_EPOLL_TRANSPORT_BRIDGE_OK");
}
