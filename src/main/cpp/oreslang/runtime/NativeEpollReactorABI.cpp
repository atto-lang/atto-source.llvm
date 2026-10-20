#include "oreslang/runtime/NativeEpollReactorABI.h"
#include "oreslang/runtime/NativeEpollReactorManager.hpp"
#include <new>
#include <utility>
using oreslang::runtime::NativeEpollReactorManager;

struct OresNativeNio {
    NativeEpollReactorManager manager;
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
    // A rejected admission must not leave an earlier live watch token in
    // caller-owned output storage. Zero it even for a null runtime.
    if (output) *output = OresNativeNioWatch{};
    if (!runtime || !output) return 0;
    auto watch = runtime->manager.try_watch(fd, events);
    if (!watch) return 0;
    *output = OresNativeNioWatch{watch->ticket.value, watch->reactor};
    return 1;
}
extern "C" int ores_native_nio_try_cancel(OresNativeNio* runtime, OresNativeNioWatch watch) {
    if (!runtime) return 0;
    return runtime->manager.try_cancel(
        NativeEpollReactorManager::Watch{watch.reactor,
            oreslang::runtime::NativeEpollReactor::Ticket{watch.ticket}}) ? 1 : 0;
}
extern "C" int ores_native_nio_try_pop(OresNativeNio* runtime, OresNativeNioCompletion* output) {
    // A denied pop must not leave an earlier terminal's ticket/FD in the
    // caller's buffer. Preserve the same fail-closed output contract as
    // try_watch, including a missing runtime and nonparking lock denial.
    if (output) *output = OresNativeNioCompletion{};
    if (!runtime || !output) return 0;
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
