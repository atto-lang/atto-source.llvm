#include "oreslang/runtime/NativeOresVMNio.h"

// The callback is owned by a C-compiled VM, so use C language linkage
// explicitly rather than relying on permissive C++ callback-pointer ABI.
extern "C" void ores_native_vm_nio_destroy_backend(void* backend) {
    ores_native_nio_close(static_cast<OresNativeNio*>(backend));
}
namespace {
/* The trusted host owns the VM object for the duration of this call. The
 * pinned backend remains alive across concurrent retirement and host close.
 * No dynamically allocated operation frame or guest callback is involved.
 */
static bool on_vm_actor_carrier(OresNativeVM* vm) noexcept {
    if (!vm) return false;
    OresNativeExecutionContext current{};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        if (ores_native_vm_current_execution_context(
                vm, static_cast<enum OresNativeVMDomain>(i), &current))
            return true;
    }
    return false;
}

class NioBackendLease final {
public:
    explicit NioBackendLease(OresNativeVM* vm)
        : vm_(vm), ptr_(static_cast<OresNativeNio*>(ores_native_vm_nio_acquire(vm))) {}
    ~NioBackendLease() {
        if (ptr_) ores_native_vm_nio_release(vm_, ptr_);
    }
    NioBackendLease(const NioBackendLease&) = delete;
    NioBackendLease& operator=(const NioBackendLease&) = delete;
    OresNativeNio* get() const noexcept { return ptr_; }
private:
    OresNativeVM* vm_;
    OresNativeNio* ptr_;
};
}

extern "C" int ores_native_vm_enable_nio(
    OresNativeVM* vm, size_t max_reactors,
    int preallocated_realtime, size_t rt_reactors) {
    // Creating epoll fds and pthreads is permitted only during bootstrap.
    // A failed or already-retired attach destroys the candidate immediately.
    if (!vm || !ores_native_vm_nio_begin_bootstrap(vm)) return 0;
    OresNativeNio* backend =
        ores_native_nio_open(max_reactors, preallocated_realtime, rt_reactors);
    if (!backend) {
        ores_native_vm_nio_cancel_bootstrap(vm);
        return 0;
    }
    if (!ores_native_vm_attach_nio(vm, backend, &ores_native_vm_nio_destroy_backend)) {
        // attach_nio always consumes this thread's reservation. Another
        // closer may reclaim the VM immediately afterwards, so do not
        // invoke a second VM method here; reclaim only the unowned candidate.
        ores_native_nio_close(backend);
        return 0;
    }
    return 1;
}

extern "C" size_t ores_native_vm_nio_started(OresNativeVM* vm) {
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    return backend ? ores_native_nio_started_reactors(backend) : 0;
}
extern "C" int ores_native_vm_nio_try_watch(
    OresNativeVM* vm, int fd, uint32_t events, OresNativeNioWatch* output) {
    // Bare VM NIO is trusted outer-host plumbing. Actor/source lowering must
    // use an actor-owned resource-loan bridge; fail closed if a carrier tries
    // to bypass physical DONE ownership through this host-only entry point.
    if (on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    return backend ? ores_native_nio_try_watch(backend, fd, events, output) : 0;
}
extern "C" int ores_native_vm_nio_try_cancel(
    OresNativeVM* vm, OresNativeNioWatch watch) {
    if (on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    return backend ? ores_native_nio_try_cancel(backend, watch) : 0;
}
extern "C" int ores_native_vm_nio_try_pop(
    OresNativeVM* vm, OresNativeNioCompletion* output) {
    if (on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    return backend ? ores_native_nio_try_pop(backend, output) : 0;
}
extern "C" size_t ores_native_vm_nio_try_pop_batch(
    OresNativeVM* vm, OresNativeNioCompletion* output, size_t capacity) {
    if (!output || capacity == 0 || on_vm_actor_carrier(vm)) return 0;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (!backend) return 0;
    // Four reactors x 64 preallocated completions. A hostile caller cannot
    // turn one scheduler poll into an unbounded hot-path spin.
    constexpr size_t max_batch = 256;
    const size_t limit = capacity < max_batch ? capacity : max_batch;
    size_t count = 0;
    while (count < limit && ores_native_nio_try_pop(backend, &output[count]))
        ++count;
    return count;
}

extern "C" int ores_native_vm_nio_completion_fd(
    OresNativeVM* vm, size_t reactor) {
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    return backend ? ores_native_nio_completion_fd(backend, reactor) : -1;
}
extern "C" void ores_native_vm_nio_drain_signal(
    OresNativeVM* vm, size_t reactor) {
    if (on_vm_actor_carrier(vm)) return;
    NioBackendLease lease(vm);
    auto* backend = lease.get();
    if (backend) ores_native_nio_drain_completion_signal(backend, reactor);
}
