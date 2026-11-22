#include "oreslang/runtime/OresVM.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>

using oreslang::runtime::OresVM;
using oreslang::runtime::OresVmLimits;
using oreslang::runtime::IsolatePhase;

static std::atomic<bool> entered{false};
static std::atomic<bool> unblock{false};

extern "C" std::int32_t blocked_guest(OresIsolateContext* ctx,
                                       std::int64_t input, std::int64_t* out) {
    if (!ctx || !out || ctx->abi_version != ORES_ISOLATE_ABI_VERSION)
        return ORES_GUEST_BAD_CONTEXT;
    entered.store(true, std::memory_order_release);
    while (!unblock.load(std::memory_order_acquire))
        std::this_thread::yield();
    *out = input;
    return ORES_GUEST_OK;
}

extern "C" std::int32_t immediate_guest(OresIsolateContext* ctx,
                                         std::int64_t input, std::int64_t* out) {
    if (!ctx || !out || ctx->abi_version != ORES_ISOLATE_ABI_VERSION)
        return ORES_GUEST_BAD_CONTEXT;
    *out = input;
    return ORES_GUEST_OK;
}

int main() {
    // Queue a call behind a running guest, then shut down before the queued
    // call can obtain its turn. Both calls are pinned for drain accounting.
    OresVM vm(OresVmLimits{2, 256, 3});
    auto a = vm.create_isolate({"tenant-a", 128, 1,
        oreslang::runtime::IsolateMode::TrustedInProcess, 2});
    auto b = vm.create_isolate({"tenant-b", 128, 1});
    vm.start(a);
    vm.start(b);

    std::atomic<bool> queued_cancelled{false};
    std::thread first([&] {
        assert(vm.invoke_guest(a, blocked_guest, 41) == 41);
    });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::thread second([&] {
        try {
            (void)vm.invoke_guest(a, immediate_guest, 99);
        } catch (const std::logic_error&) {
            queued_cancelled.store(true, std::memory_order_release);
        }
    });
    while (vm.snapshot(a).active_guest_calls != 2)
        std::this_thread::yield();

    // Per-tenant queue limit is enforced, and another tenant stays live.
    bool denied = false;
    try { (void)vm.invoke_guest(a, immediate_guest, 100); }
    catch (const std::logic_error&) { denied = true; }
    assert(denied);
    assert(vm.invoke_guest(b, immediate_guest, 7) == 7);

    vm.begin_shutdown(a);
    assert(vm.snapshot(a).phase == IsolatePhase::Quiescing);
    bool drained_too_early = false;
    try { vm.complete_shutdown(a); }
    catch (const std::logic_error&) { drained_too_early = true; }
    assert(drained_too_early);

    unblock.store(true, std::memory_order_release);
    first.join();
    second.join();
    assert(queued_cancelled.load(std::memory_order_acquire));
    assert(vm.snapshot(a).active_guest_calls == 0);
    vm.complete_shutdown(a);
    vm.destroy(a);

    // Recreating an identical tenant ID must not resurrect an old handle.
    auto again = vm.create_isolate({"tenant-a", 128, 1});
    vm.start(again);
    bool stale = false;
    try { (void)vm.invoke_guest(a, immediate_guest, 5); }
    catch (const std::logic_error&) { stale = true; }
    assert(stale);
    assert(vm.invoke_guest(again, immediate_guest, 8) == 8);
    vm.begin_shutdown(again);
    vm.complete_shutdown(again);
    vm.destroy(again);
    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);
}
