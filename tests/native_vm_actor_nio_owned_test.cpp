#include "oreslang/runtime/NativeOresVMActorNio.hpp"
#include "oreslang/runtime/NativeOresVMNio.h"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

using namespace oreslang::runtime;
extern "C" void no_guest(void*, size_t, uint64_t) { std::abort(); }

static OresNativeVMConfig configuration() {
    OresNativeVMConfig cfg{};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = 2;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = 1;
        cfg.domains[i].on_message = &no_guest;
    }
    return cfg;
}
static OresNativeNioWatch watch(OresNativeVM* vm,
                                NativeTransportCallbackGate& gate, int fd) {
    for (int n = 0; n < 3000; ++n) {
        OresNativeNioWatch result{};
        if (vm_nio_actor_try_watch(vm, gate, fd, EPOLLIN, &result)) {
            assert(result.ticket);
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "timed out registering owned VM NIO watch");
    return {};
}
static ActorNioCompletion pop(OresNativeVM* vm) {
    for (int n = 0; n < 3000; ++n) {
        ActorNioCompletion result{};
        if (vm_nio_actor_try_pop(vm, &result)) {
            assert(result.lease_id != 0);
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "timed out consuming owned VM NIO completion");
    return {};
}
static bool cancel(OresNativeVM* vm, OresNativeNioWatch watch_value) {
    for (int n = 0; n < 3000; ++n) {
        if (vm_nio_actor_try_cancel(vm, watch_value)) return true;
        std::this_thread::yield();
    }
    return false;
}

int main() {
    auto cfg = configuration();
    auto* vm = ores_native_vm_open(&cfg);
    assert(vm != nullptr);
    assert(ores_native_vm_enable_nio(vm, 1, 0, 1));
    NativeTransportCallbackGate gate(17, 19, 23, 29);
    int p[2]{-1, -1};
    assert(::pipe2(p, O_NONBLOCK | O_CLOEXEC) == 0);
    const auto w = watch(vm, gate, p[0]);
    // The irreversible actor-owned mode prevents a legacy C consumer
    // from silently consuming a record without its callback permit.
    OresNativeNioCompletion raw{};
    assert(!ores_native_vm_nio_try_pop(vm, &raw));
    assert(!ores_native_vm_nio_try_cancel(vm, w));
    assert(::write(p[1], "x", 1) == 1);
    auto completed = pop(vm);
    assert(completed.event.outcome == ORES_NATIVE_NIO_READY);
    assert(completed.event.watch.ticket == w.ticket);
    assert(completed.owner.slab == gate.binding().slab);
    gate.stop_admission();
    assert(!gate.try_drain());
    assert(!vm_nio_actor_try_watch(vm, gate, p[0], EPOLLIN, &raw.watch));
    auto forged = completed;
    ++forged.owner.slab;
    assert(!vm_nio_actor_acknowledge(vm, forged));
    forged = completed;
    ++forged.lease_id;
    assert(!vm_nio_actor_acknowledge(vm, forged));
    forged = completed;
    forged.event.fd = -1;
    assert(!vm_nio_actor_acknowledge(vm, forged));
    forged = completed;
    forged.event.events ^= EPOLLOUT;
    assert(!vm_nio_actor_acknowledge(vm, forged));
    forged = completed;
    forged.event.outcome = ORES_NATIVE_NIO_STOPPED;
    assert(!vm_nio_actor_acknowledge(vm, forged));

    // The reactor has already popped this readiness and may accept another
    // watch of its FD. But the actor-owned ledger must retain exclusive FD
    // ownership until physical callback acknowledgment.
    NativeTransportCallbackGate overlap_gate(17, 19, 99, 119);
    OresNativeNioWatch overlap{};
    assert(!vm_nio_actor_try_watch(vm, overlap_gate, p[0], EPOLLIN, &overlap));
    assert(!gate.try_drain());

    // The downstream carrier owns the completion until its last actor
    // memory access. Releasing the receipt early is not sufficient.
    std::atomic<bool> entered{false};
    std::atomic<bool> finish{false};
    std::thread consumer([&] {
        entered.store(true, std::memory_order_release);
        while (!finish.load(std::memory_order_acquire))
            std::this_thread::yield();
        assert(vm_nio_actor_acknowledge(vm, completed));
    });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    assert(!gate.try_drain());
    finish.store(true, std::memory_order_release);
    consumer.join();
    assert(!vm_nio_actor_acknowledge(vm, completed)); // double completion
    assert(gate.try_drain());
    // The same FD may be rearmed only AFTER the previous owner has actually
    // acknowledged its completion. The readiness byte remains unread.
    const auto rearmed = watch(vm, overlap_gate, p[0]);
    assert(rearmed.ticket != w.ticket);
    auto renewed = pop(vm);
    assert(renewed.event.watch.ticket == rearmed.ticket);
    assert(renewed.event.outcome == ORES_NATIVE_NIO_READY);
    overlap_gate.stop_admission();
    assert(!overlap_gate.try_drain());
    assert(vm_nio_actor_acknowledge(vm, renewed));
    assert(overlap_gate.try_drain());
    ::close(p[0]); ::close(p[1]);

    // Cancelled terminal work must also be acknowledged; the old C API
    // remains unable to consume these actor-owned completions.
    NativeTransportCallbackGate cancel_gate(17, 19, 24, 30);
    int c[2]{-1, -1};
    assert(::pipe2(c, O_NONBLOCK | O_CLOEXEC) == 0);
    const auto cancelled_watch = watch(vm, cancel_gate, c[0]);
    assert(cancel(vm, cancelled_watch));
    assert(!vm_nio_actor_try_cancel(vm, cancelled_watch));
    auto cancelled = pop(vm);
    assert(cancelled.event.outcome == ORES_NATIVE_NIO_CANCELLED);
    cancel_gate.stop_admission();
    assert(!cancel_gate.try_drain());
    assert(vm_nio_actor_acknowledge(vm, cancelled));
    assert(cancel_gate.try_drain());
    ::close(c[0]); ::close(c[1]);

    // VM-backed native NIO stop is host-only and joins the reactor. The
    // STOPPED terminal envelope still holds a callback permit until ack.
    NativeTransportCallbackGate stop_gate(17, 19, 25, 31);
    int s[2]{-1, -1};
    assert(::pipe2(s, O_NONBLOCK | O_CLOEXEC) == 0);
    const auto stopped_watch = watch(vm, stop_gate, s[0]);
    stop_gate.stop_admission();
    assert(vm_nio_actor_stop(vm));
    assert(!stop_gate.try_drain());
    auto stopped = pop(vm);
    assert(stopped.event.watch.ticket == stopped_watch.ticket);
    assert(stopped.event.outcome == ORES_NATIVE_NIO_STOPPED);
    assert(vm_nio_actor_acknowledge(vm, stopped));
    assert(stop_gate.try_drain());
    ::close(s[0]); ::close(s[1]);

    assert(ores_native_vm_close(vm));

    // A backend that selected the legacy C mode must never switch to
    // owned completion consumption, including when its queue is empty.
    auto* legacy = ores_native_nio_open(1, 0, 1);
    assert(legacy);
    assert(!ores_native_nio_try_pop(legacy, &raw)); // selects C mode
    NativeTransportCallbackGate foreign(17, 19, 26, 32);
    OresNativeNioWatch blocked{};
    assert(!nio_actor_try_watch(legacy, foreign, -1, EPOLLIN, &blocked));
    foreign.stop_admission();
    assert(foreign.try_drain());
    ores_native_nio_close(legacy);
    std::puts("NATIVE_VM_ACTOR_NIO_OWNED_OK");
}
