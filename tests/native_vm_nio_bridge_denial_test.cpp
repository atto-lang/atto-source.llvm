#include "oreslang/runtime/NativeOresVMNio.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sys/epoll.h>

// This tests the real C++ VM bridge and real C ABI implementation, but uses
// deliberately unavailable trusted-VM lifecycle test doubles. It is NOT a
// substitute for NativeOresVM.c + NativeActorScheduler.c integration tests.
namespace {
std::atomic<unsigned> acquisitions{0};
std::atomic<unsigned> bad_releases{0};
}
extern "C" int ores_native_vm_nio_begin_bootstrap(OresNativeVM*) { return 0; }
extern "C" void ores_native_vm_nio_cancel_bootstrap(OresNativeVM*) { std::abort(); }
extern "C" int ores_native_vm_attach_nio(
    OresNativeVM*, void*, OresNativeVMNioDestroy) { std::abort(); }
extern "C" void* ores_native_vm_nio_acquire(OresNativeVM*) {
    acquisitions.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
extern "C" void ores_native_vm_nio_release(OresNativeVM*, void*) {
    bad_releases.fetch_add(1, std::memory_order_relaxed);
    std::abort();
}
static void require(bool good) {
    if (!good) std::abort();
}
int main() {
    alignas(std::max_align_t) unsigned char dummy_vm[64]{};
    auto* vm = reinterpret_cast<OresNativeVM*>(dummy_vm);
    require(!ores_native_vm_enable_nio(vm, 1, 0, 1));
    require(ores_native_vm_nio_started(vm) == 0);
    for (unsigned round = 0; round < 1000; ++round) {
        // No backend was ever attached. Every rejection must erase a caller's
        // stale successful-looking ticket, including the null-VM case.
        OresNativeNioWatch watch{UINT64_MAX, UINT8_MAX};
        require(!ores_native_vm_nio_try_watch(vm, -1, EPOLLIN, &watch));
        require(watch.ticket == 0 && watch.reactor == 0);
        watch = OresNativeNioWatch{UINT64_MAX, UINT8_MAX};
        require(!ores_native_vm_nio_try_watch(nullptr, -1, EPOLLIN, &watch));
        require(watch.ticket == 0 && watch.reactor == 0);
        require(!ores_native_vm_nio_try_watch(vm, -1, EPOLLIN, nullptr));
        require(!ores_native_vm_nio_try_cancel(
            vm, OresNativeNioWatch{UINT64_MAX, UINT8_MAX}));
        OresNativeNioCompletion done{};
        require(!ores_native_vm_nio_try_pop(vm, &done));
        require(ores_native_vm_nio_try_pop_batch(vm, &done, 1) == 0);
        require(ores_native_vm_nio_completion_fd(vm, 0) == -1);
        ores_native_vm_nio_drain_signal(vm, 0);
    }
    OresNativeNioWatch c_watch{UINT64_MAX, UINT8_MAX};
    require(!ores_native_nio_try_watch(nullptr, -1, EPOLLIN, &c_watch));
    require(c_watch.ticket == 0 && c_watch.reactor == 0);
    require(acquisitions.load(std::memory_order_relaxed) >= 6000);
    require(bad_releases.load(std::memory_order_relaxed) == 0);
    return 0;
}
