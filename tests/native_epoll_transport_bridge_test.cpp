#include "oreslang/runtime/NativeEpollTransportBridge.hpp"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <atomic>
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

static std::optional<NativeEpollTransportBridge::Ticket>
await_watch(NativeEpollTransportBridge& bridge, int fd) {
    for (int i = 0; i < 1000; ++i) {
        auto watch = bridge.try_watch(fd, EPOLLIN);
        if (watch) return watch;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

static bool await_cancel(NativeEpollTransportBridge& bridge,
                         NativeEpollTransportBridge::Ticket ticket) {
    for (int i = 0; i < 1000; ++i) {
        if (bridge.try_cancel(ticket)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

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
        auto watch = await_watch(bridge, fds[0]);
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

            // A downstream actor carrier may retain the *whole* completion,
            // not just its integer readiness ticket. Storage cannot be
            // reclaimed while that carrier still owns the moved envelope.
            std::atomic<bool> entered{false};
            std::atomic<bool> finish{false};
            std::thread consumer([lease = std::move(*completed),
                                  &entered, &finish]() mutable {
                entered.store(true, std::memory_order_release);
                while (!finish.load(std::memory_order_acquire))
                    std::this_thread::yield();
                assert(lease.permit);
                lease.permit.release();
            });
            completed.reset();
            while (!entered.load(std::memory_order_acquire))
                std::this_thread::yield();
            assert(!gate.try_drain());
            finish.store(true, std::memory_order_release);
            consumer.join();
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
        auto ticket = await_watch(bridge, cancelled[0]);
        assert(ticket);
        assert(await_cancel(bridge, *ticket));
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

    // Shutdown without a readiness/cancellation event still publishes
    // exactly one stopped completion; no permit may be lost or released on
    // the reactor thread before a consumer observes that terminal result.
    int stopped[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0, stopped) == 0);
    NativeTransportCallbackGate stop_gate(7, 3, 13, 93);
    {
        NativeEpollTransportBridge bridge(stop_gate);
        assert(bridge.start());
        auto ticket = await_watch(bridge, stopped[0]);
        assert(ticket);
        bridge.stop();
        assert(!stop_gate.try_drain());
        {
            auto completed = await_completion(bridge);
            assert(completed && completed->permit);
            assert(completed->event.ticket.value == ticket->value);
            assert(completed->event.outcome ==
                NativeEpollTransportBridge::Reactor::Outcome::stopped);
            assert(!stop_gate.try_drain());
        }
        assert(stop_gate.try_drain());
    }
    (void)::close(stopped[0]);
    (void)::close(stopped[1]);

    std::puts("NATIVE_EPOLL_TRANSPORT_BRIDGE_OK");
}
