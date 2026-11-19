#include "oreslang/runtime/NativeActorNioCallbacks.hpp"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

using namespace oreslang::runtime;

static OresNativeNioWatch watch(NativeActorNioCallbacks& ledger,
        NativeEpollReactorManager& manager, NativeTransportCallbackGate& gate, int fd) {
    for (int i = 0; i < 3000; ++i) {
        OresNativeNioWatch value{};
        if (ledger.try_watch(manager, gate, fd, EPOLLIN, &value)) return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::abort();
}
static NativeActorNioCallbacks::Completion pop(NativeActorNioCallbacks& ledger,
        NativeEpollReactorManager& manager) {
    for (int i = 0; i < 3000; ++i) {
        NativeActorNioCallbacks::Completion value{};
        if (ledger.try_pop(manager, &value)) return value;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::abort();
}

int main() {
    NativeEpollReactorManager manager;
    NativeActorNioCallbacks ledger;
    NativeTransportCallbackGate first_gate(3, 4, 5, 6);
    NativeTransportCallbackGate second_gate(3, 4, 6, 7);
    int fds[2] = {-1, -1};
    assert(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
    auto first = watch(ledger, manager, first_gate, fds[0]);
    assert(::write(fds[1], "a", 1) == 1);
    auto completed = pop(ledger, manager);
    assert(completed.event.watch.ticket == first.ticket);
    assert(completed.event.outcome == ORES_NATIVE_NIO_READY);
    first_gate.stop_admission();
    assert(!first_gate.try_drain());

    // The reactor has already released its kernel FD claim, but the actor
    // completion still owns this descriptor until terminal acknowledgment.
    OresNativeNioWatch rejected{};
    assert(!ledger.try_watch(manager, second_gate, fds[0], EPOLLIN, &rejected));
    auto altered = completed;
    altered.event.fd = -1;
    assert(!ledger.acknowledge(altered));
    altered = completed;
    altered.event.events ^= EPOLLOUT;
    assert(!ledger.acknowledge(altered));
    altered = completed;
    altered.event.outcome = ORES_NATIVE_NIO_STOPPED;
    assert(!ledger.acknowledge(altered));
    assert(!first_gate.try_drain());

    // Only the exact terminal acknowledgment releases the old FD loan.
    assert(ledger.acknowledge(completed));
    assert(!ledger.acknowledge(completed));
    assert(first_gate.try_drain());
    auto second = watch(ledger, manager, second_gate, fds[0]);
    assert(second.ticket != first.ticket);
    auto rearmed = pop(ledger, manager);
    assert(rearmed.event.watch.ticket == second.ticket);
    assert(rearmed.event.outcome == ORES_NATIVE_NIO_READY);
    second_gate.stop_admission();
    assert(!second_gate.try_drain());
    assert(ledger.acknowledge(rearmed));
    assert(second_gate.try_drain());
    manager.stop();
    ::close(fds[0]); ::close(fds[1]);
    std::puts("NATIVE_ACTOR_NIO_FD_LEASE_OK");
}
