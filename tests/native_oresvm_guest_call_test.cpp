#include "oreslang/runtime/OresVM.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>
using oreslang::runtime::OresVM;
using oreslang::runtime::IsolatePhase;
template<class F> bool rejects(F&& f) { try { f(); } catch(const std::exception&) { return true; } return false; }
extern "C" std::int32_t guest_sum(OresIsolateContext* ctx, std::int64_t delta, std::int64_t* out) {
    if (!ctx || !out || ctx->abi_version != ORES_ISOLATE_ABI_VERSION ||
        ctx->slot_count != ORES_ISOLATE_I64_SLOTS) return ORES_GUEST_BAD_CONTEXT;
    std::int64_t old = 0;
    if (ctx->load_i64(ctx->call_data, 0, &old) != 1) return ORES_GUEST_LOAD_DENIED;
    if ((delta > 0 && old > INT64_MAX - delta) ||
        (delta < 0 && old < INT64_MIN - delta)) return ORES_GUEST_OVERFLOW;
    const auto next = old + delta;
    if (ctx->store_i64(ctx->call_data, 0, next) != 1) return ORES_GUEST_STORE_DENIED;
    *out = next;
    return ORES_GUEST_OK;
}
static std::atomic<bool> entered{false}, allow_exit{false};
extern "C" std::int32_t guest_wait(OresIsolateContext* ctx, std::int64_t delta, std::int64_t* out) {
    entered.store(true, std::memory_order_release);
    while(!allow_exit.load(std::memory_order_acquire)) std::this_thread::yield();
    return guest_sum(ctx, delta, out);
}
extern "C" std::int32_t guest_throw(OresIsolateContext*, std::int64_t, std::int64_t*) {
    throw std::runtime_error("host-only test exception");
}
int main() {
    OresVM vm;
    auto a = vm.create_isolate({"tenant-a", 1024, 2});
    auto b = vm.create_isolate({"tenant-b", 1024, 2});
    vm.start(a); vm.start(b);
    assert(vm.invoke_guest(a, guest_sum, -1) == -1);
    assert(vm.invoke_guest(a, guest_sum, -2) == -3);
    assert(vm.invoke_guest(a, guest_sum, -3) == -6);
    assert(vm.invoke_guest(a, guest_sum, 6) == 0);
    assert(vm.invoke_guest(a, guest_sum, 7) == 7);
    assert(vm.invoke_guest(a, guest_sum, 4) == 11);
    assert(vm.invoke_guest(b, guest_sum, 19) == 19);
    assert(vm.invoke_guest(a, guest_sum, 0) == 11);
    const auto extreme = vm.invoke_guest_result(a, guest_sum, INT64_MAX);
    assert(extreme.status == ORES_GUEST_OVERFLOW && extreme.value == 0);
    assert(vm.invoke_guest(a, guest_sum, 0) == 11);
    const auto bad = vm.invoke_guest_result(a, [](OresIsolateContext*, std::int64_t, std::int64_t* out) -> std::int32_t {
        *out = -3;
        return ORES_GUEST_LOAD_DENIED;
    }, 0);
    assert(bad.status == ORES_GUEST_LOAD_DENIED && bad.value == 0);
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
    // Native exceptions are not ordinary returned guest error codes.
    // Quarantine the possibly partially-mutated tenant and require a
    // complete stop/destroy plus new generation before state is usable.
    auto fault = vm.create_isolate({"fault-tenant", 1024, 1});
    vm.start(fault);
    assert(vm.invoke_guest(fault, guest_sum, 9) == 9);
    assert(rejects([&]{ vm.invoke_guest(fault, guest_throw, 0); }));
    assert(vm.snapshot(fault).active_guest_calls == 0);
    assert(vm.snapshot(fault).phase == IsolatePhase::Quiescing);
    assert(rejects([&]{ vm.invoke_guest(fault, guest_sum, 1); }));
    vm.complete_shutdown(fault);
    vm.destroy(fault);
    auto fault2 = vm.create_isolate({"fault-tenant", 1024, 1});
    vm.start(fault2);
    assert(vm.invoke_guest(fault2, guest_sum, 1) == 1);
    assert(rejects([&]{ vm.snapshot(fault); }));
    vm.begin_shutdown(fault2); vm.complete_shutdown(fault2); vm.destroy(fault2);
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
