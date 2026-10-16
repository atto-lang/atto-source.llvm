#include "oreslang/runtime/NativeOresVMNio.h"

// The callback is owned by a C-compiled VM, so use C language linkage
// explicitly rather than relying on permissive C++ callback-pointer ABI.
extern "C" void ores_native_vm_nio_destroy_backend(void* backend) {
    ores_native_nio_close(static_cast<OresNativeNio*>(backend));
}
namespace {
OresNativeNio* admitted_nio(OresNativeVM* vm) {
    return static_cast<OresNativeNio*>(ores_native_vm_nio_backend(vm));
}
}

extern "C" int ores_native_vm_enable_nio(
    OresNativeVM* vm, size_t max_reactors,
    int preallocated_realtime, size_t rt_reactors) {
    // Creating epoll fds and pthreads is permitted only during bootstrap.
    // A failed or already-retired attach destroys the candidate immediately.
    if (!vm) return 0;
    OresNativeNio* backend =
        ores_native_nio_open(max_reactors, preallocated_realtime, rt_reactors);
    if (!backend) return 0;
    if (!ores_native_vm_attach_nio(vm, backend, &ores_native_vm_nio_destroy_backend)) {
        ores_native_nio_close(backend);
        return 0;
    }
    return 1;
}

extern "C" size_t ores_native_vm_nio_started(OresNativeVM* vm) {
    auto* backend = admitted_nio(vm);
    return backend ? ores_native_nio_started_reactors(backend) : 0;
}
extern "C" int ores_native_vm_nio_try_watch(
    OresNativeVM* vm, int fd, uint32_t events, OresNativeNioWatch* output) {
    auto* backend = admitted_nio(vm);
    return backend ? ores_native_nio_try_watch(backend, fd, events, output) : 0;
}
extern "C" int ores_native_vm_nio_try_cancel(
    OresNativeVM* vm, OresNativeNioWatch watch) {
    auto* backend = admitted_nio(vm);
    return backend ? ores_native_nio_try_cancel(backend, watch) : 0;
}
extern "C" int ores_native_vm_nio_try_pop(
    OresNativeVM* vm, OresNativeNioCompletion* output) {
    auto* backend = admitted_nio(vm);
    return backend ? ores_native_nio_try_pop(backend, output) : 0;
}
extern "C" int ores_native_vm_nio_completion_fd(
    OresNativeVM* vm, size_t reactor) {
    auto* backend = admitted_nio(vm);
    return backend ? ores_native_nio_completion_fd(backend, reactor) : -1;
}
extern "C" void ores_native_vm_nio_drain_signal(
    OresNativeVM* vm, size_t reactor) {
    auto* backend = admitted_nio(vm);
    if (backend) ores_native_nio_drain_completion_signal(backend, reactor);
}
