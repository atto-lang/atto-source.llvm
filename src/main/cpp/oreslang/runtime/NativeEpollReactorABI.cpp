#include "oreslang/runtime/NativeEpollReactorABI.h"
#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include "oreslang/runtime/NativeOresVMActorNio.hpp"
#include <atomic>
#include <new>
#include <utility>
using oreslang::runtime::NativeEpollReactorManager;

struct OresNativeNio {
    NativeEpollReactorManager manager;
    oreslang::runtime::NativeActorNioCallbacks owned;
    // Irreversible per-backend mode prevents raw legacy C completions from
    // consuming actor-owned envelopes and silently losing their permits.
    // 0 = unselected, 1 = legacy, 2 = actor-owned.
    std::atomic<unsigned> mode{0};
    bool select_mode(unsigned requested) noexcept {
        unsigned expected = 0;
        return mode.compare_exchange_strong(expected, requested,
                std::memory_order_acq_rel, std::memory_order_acquire)
            || expected == requested;
    }
    bool is_mode(unsigned requested) const noexcept {
        return mode.load(std::memory_order_acquire) == requested;
    }
    OresNativeNio(size_t max_reactors, bool realtime, size_t rt_reactors)
        : manager(max_reactors,
            realtime ? NativeEpollReactorManager::Profile::preallocated_rt
                     : NativeEpollReactorManager::Profile::throughput,
            rt_reactors) {}
};

extern "C" OresNativeNio* ores_native_nio_open(
    size_t max_reactors, int realtime_preallocated, size_t rt_reactors) {
    try { return new OresNativeNio(max_reactors, realtime_preallocated != 0, rt_reactors); }
    catch (...) { return nullptr; } // no C++ exception crosses the runtime ABI
}
extern "C" size_t ores_native_nio_started_reactors(const OresNativeNio* runtime) {
    return runtime ? runtime->manager.started_threads() : 0;
}
extern "C" int ores_native_nio_try_watch(
    OresNativeNio* runtime, int fd, uint32_t events, OresNativeNioWatch* output) {
    if (!runtime || !output || !runtime->select_mode(1)) return 0;
    auto watch = runtime->manager.try_watch(fd, events);
    if (!watch) return 0;
    *output = OresNativeNioWatch{watch->ticket.value, watch->reactor};
    return 1;
}
extern "C" int ores_native_nio_try_cancel(OresNativeNio* runtime, OresNativeNioWatch watch) {
    if (!runtime || !runtime->is_mode(1)) return 0;
    return runtime->manager.try_cancel(
        NativeEpollReactorManager::Watch{watch.reactor,
            oreslang::runtime::NativeEpollReactor::Ticket{watch.ticket}}) ? 1 : 0;
}
extern "C" int ores_native_nio_try_pop(OresNativeNio* runtime, OresNativeNioCompletion* output) {
    if (!runtime || !output || !runtime->select_mode(1)) return 0;
    auto completion = runtime->manager.try_pop();
    if (!completion) return 0;
    OresNativeNioOutcome outcome;
    switch (completion->event.outcome) {
        case oreslang::runtime::NativeEpollReactor::Outcome::ready:
            outcome = ORES_NATIVE_NIO_READY; break;
        case oreslang::runtime::NativeEpollReactor::Outcome::cancelled:
            outcome = ORES_NATIVE_NIO_CANCELLED; break;
        case oreslang::runtime::NativeEpollReactor::Outcome::stopped:
            outcome = ORES_NATIVE_NIO_STOPPED; break;
        default: return 0;
    }
    *output = OresNativeNioCompletion{
        {completion->event.ticket.value, completion->reactor},
        completion->event.fd, completion->event.events, outcome};
    return 1;
}
extern "C" int ores_native_nio_completion_fd(const OresNativeNio* runtime, size_t reactor) {
    return runtime ? runtime->manager.completion_fd(reactor) : -1;
}
extern "C" void ores_native_nio_drain_completion_signal(OresNativeNio* runtime, size_t reactor) {
    if (runtime) runtime->manager.drain_completion_signal(reactor);
}
extern "C" void ores_native_nio_close(OresNativeNio* runtime) {
    if (runtime) {
        runtime->manager.stop();
        delete runtime;
    }
}

namespace oreslang::runtime {
// Legacy NIO and actor-owned NIO never share a completion-consumption mode.
// Both use the VM's existing bounded NativeEpollReactorManager; the actor-owned
// ledger only retains counted callback permits through final acknowledgment.
bool nio_actor_try_watch(OresNativeNio* backend,
        NativeTransportCallbackGate& owner, int fd, std::uint32_t events,
        OresNativeNioWatch* output) noexcept {
    if (!backend || !output || !backend->select_mode(2)) return false;
    return backend->owned.try_watch(backend->manager, owner, fd, events, output);
}
bool nio_actor_try_cancel(OresNativeNio* backend,
                          OresNativeNioWatch watch) noexcept {
    return backend && backend->is_mode(2) &&
        backend->owned.try_cancel(backend->manager, watch);
}
bool nio_actor_stop(OresNativeNio* backend) noexcept {
    if (!backend || !backend->is_mode(2)) return false;
    backend->manager.stop(); // trusted host only, may join pthreads
    return true;
}
bool nio_actor_try_pop(OresNativeNio* backend,
                       ActorNioCompletion* output) noexcept {
    return backend && backend->is_mode(2) &&
        backend->owned.try_pop(backend->manager, output);
}
bool nio_actor_acknowledge(OresNativeNio* backend,
                           const ActorNioCompletion& completion) noexcept {
    return backend && backend->is_mode(2) &&
        backend->owned.acknowledge(completion);
}
} // namespace oreslang::runtime
