#include "oreslang/runtime/OresVM.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>
using oreslang::runtime::OresVM;
using oreslang::runtime::IsolatePhase;
template<class F> bool rejects(F&& f) { try { f(); } catch(const std::exception&) { return true; } return false; }
extern "C" std::int64_t guest_sum(OresIsolateContext* ctx, std::int64_t delta) {
    if (!ctx || ctx->abi_version != ORES_ISOLATE_ABI_VERSION ||
        ctx->slot_count != ORES_ISOLATE_I64_SLOTS) return -1;
    std::int64_t old = 0;
    if (ctx->load_i64(ctx->call_data, 0, &old) != 1) return -2;
    if (delta > 0 && old > INT64_MAX - delta) return -3;
    const auto next = old + delta;
    if (ctx->store_i64(ctx->call_data, 0, next) != 1) return -4;
    return next;
}
static std::atomic<bool> entered{false}, allow_exit{false};
extern "C" std::int64_t guest_wait(OresIsolateContext* ctx, std::int64_t delta) {
    entered.store(true, std::memory_order_release);
    while(!allow_exit.load(std::memory_order_acquire)) std::this_thread::yield();
    return guest_sum(ctx, delta);
}
extern "C" std::int64_t guest_throw(OresIsolateContext*, std::int64_t) {
    throw std::runtime_error("host-only test exception");
}
int main() {
    OresVM vm;
    auto a = vm.create_isolate({"tenant-a", 1024, 2});
    auto b = vm.create_isolate({"tenant-b", 1024, 2});
    vm.start(a); vm.start(b);
    assert(vm.invoke_guest(a, guest_sum, 7) == 7);
    assert(vm.invoke_guest(a, guest_sum, 4) == 11);
    assert(vm.invoke_guest(b, guest_sum, 19) == 19);
    assert(vm.invoke_guest(a, guest_sum, 0) == 11);
    assert(vm.snapshot(a).active_guest_calls == 0);
    // Counter previously allowed lost updates: load/store are two callbacks.
    // Synchronous calls must be serialized per tenant but not across tenants.
    std::thread parallel[4];
    for (auto& worker : parallel) worker = std::thread([&] {
        for (int i = 0; i < 250; ++i)
            (void)vm.invoke_guest(b, guest_sum, 1);
    });
    for (auto& worker : parallel) worker.join();
    assert(vm.invoke_guest(b, guest_sum, 0) == 1019);
    assert(rejects([&]{ vm.invoke_guest(a, guest_throw, 0); }));
    assert(vm.snapshot(a).active_guest_calls == 0);
    assert(rejects([&]{ vm.invoke_guest(a, nullptr, 0); }));
    std::thread worker([&] { assert(vm.invoke_guest(a, guest_wait, 3) == 14); });
    while(!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    assert(vm.snapshot(a).active_guest_calls == 1);
    vm.begin_shutdown(a);
    assert(vm.snapshot(a).phase == IsolatePhase::Quiescing);
    assert(rejects([&]{ vm.invoke_guest(a, guest_sum, 0); }));
    assert(rejects([&]{ vm.complete_shutdown(a); }));
    assert(vm.invoke_guest(b, guest_sum, 1) == 1020);
    allow_exit.store(true, std::memory_order_release);
    worker.join();
    vm.complete_shutdown(a); vm.destroy(a);
    assert(rejects([&]{ vm.invoke_guest(a, guest_sum, 1); }));
    auto anew = vm.create_isolate({"tenant-a", 1024, 2});
    vm.start(anew);
    assert(vm.invoke_guest(anew, guest_sum, 2) == 2);
    assert(rejects([&]{ vm.snapshot(a); }));
    vm.begin_shutdown(anew); vm.complete_shutdown(anew); vm.destroy(anew);
    vm.begin_shutdown(b); vm.complete_shutdown(b); vm.destroy(b);
}
