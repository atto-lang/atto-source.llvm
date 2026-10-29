#include "oreslang/runtime/OresVM.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

using oreslang::runtime::IsolateMode;
using oreslang::runtime::OresVM;
using oreslang::runtime::OresVmLimits;

template <class F> bool rejects(F&& fn) {
    try { fn(); } catch (const std::exception&) { return true; }
    return false;
}

static std::atomic<bool> entered{false};
static std::atomic<bool> release_guest{false};

extern "C" std::int32_t waiting_guest(OresIsolateContext* ctx, std::int64_t value,
                                         std::int64_t* out) {
    if (!ctx || !out || ctx->abi_version != ORES_ISOLATE_ABI_VERSION)
        return ORES_GUEST_BAD_CONTEXT;
    entered.store(true, std::memory_order_release);
    while (!release_guest.load(std::memory_order_acquire))
        std::this_thread::yield();
    *out = value;
    return ORES_GUEST_OK;
}

extern "C" std::int32_t echo_guest(OresIsolateContext* ctx, std::int64_t value,
                                      std::int64_t* out) {
    if (!ctx || !out || ctx->abi_version != ORES_ISOLATE_ABI_VERSION)
        return ORES_GUEST_BAD_CONTEXT;
    *out = value;
    return ORES_GUEST_OK;
}

int main() {
    assert(rejects([] { OresVM invalid(OresVmLimits{0, 32, 2}); }));
    assert(rejects([] { OresVM invalid(OresVmLimits{2, 0, 2}); }));
    assert(rejects([] { OresVM invalid(OresVmLimits{2, 32, 0}); }));
    // Uncharged tenant ID memory and control bytes must not bypass admission.
    OresVM ids;
    assert(rejects([&] { ids.create_isolate({std::string(257, 'x'), 1, 1}); }));
    assert(rejects([&] { ids.create_isolate({std::string("a\0b", 3), 1, 1}); }));
    assert(rejects([&] { ids.create_isolate({"control\n", 1, 1}); }));
    assert(rejects([&] { ids.create_isolate({std::string("a\x7f", 2), 1, 1}); }));


    // An aggregate heap budget is independent of the tenant count limit.
    OresVM heap_budget(OresVmLimits{3, 32, 1});
    auto budget_a = heap_budget.create_isolate({"budget-a", 16, 1});
    assert(rejects([&] { heap_budget.create_isolate({"budget-b", 17, 1}); }));
    auto budget_b = heap_budget.create_isolate({"budget-b", 16, 1});
    for (const auto& h : {budget_a, budget_b}) {
        heap_budget.begin_shutdown(h);
        heap_budget.complete_shutdown(h);
        heap_budget.destroy(h);
    }

    // Root heap reservations are charged at create time, not on allocation.
    // Rejection leaves no tenant admitted and destruction returns the charge.
    OresVM vm(OresVmLimits{2, 32, 2});
    auto a = vm.create_isolate({"a", 16, 1, IsolateMode::TrustedInProcess, 1});
    auto b = vm.create_isolate({"b", 16, 1});
    assert(rejects([&] { vm.create_isolate({"c", 1, 1}); }));
    assert(rejects([&] { vm.create_isolate({"a", 1, 1}); }));
    assert(rejects([&] { vm.create_isolate({"bad", 1, 1, IsolateMode::TrustedInProcess, 0}); }));
    vm.start(a);
    vm.start(b);
    auto a_buf = vm.allocate(a, 16);
    auto b_buf = vm.allocate(b, 16);
    vm.write_byte(a, a_buf, 0, 41);
    vm.write_byte(b, b_buf, 0, 42);
    assert(vm.read_byte(a, a_buf, 0) == 41);
    assert(vm.read_byte(b, b_buf, 0) == 42);

    // Per-tenant admission must count waiting callers, not only executing
    // guest turns. Other tenants still make progress under a separate budget.
    std::thread waiter([&] { assert(vm.invoke_guest(a, waiting_guest, 81) == 81); });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    assert(vm.snapshot(a).active_guest_calls == 1);
    assert(rejects([&] { vm.invoke_guest(a, echo_guest, 90); }));
    assert(vm.invoke_guest(b, echo_guest, 37) == 37); // independent progress
    vm.begin_shutdown(a);
    assert(rejects([&] { vm.complete_shutdown(a); }));
    release_guest.store(true, std::memory_order_release);
    waiter.join();
    assert(vm.snapshot(a).active_guest_calls == 0);
    vm.complete_shutdown(a);
    vm.destroy(a);
    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);
    auto c = vm.create_isolate({"c", 16, 1});
    vm.start(c);
    assert(rejects([&] { vm.read_byte(c, b_buf, 0); }));
    vm.begin_shutdown(c);
    vm.complete_shutdown(c);
    vm.destroy(c);

    // A separate VM-wide pin budget prevents a flood spread across tenants.
    entered.store(false, std::memory_order_release);
    release_guest.store(false, std::memory_order_release);
    OresVM limited(OresVmLimits{2, 64, 1});
    auto x = limited.create_isolate({"x", 32, 1});
    auto y = limited.create_isolate({"y", 32, 1});
    limited.start(x);
    limited.start(y);
    std::thread busy([&] { assert(limited.invoke_guest(x, waiting_guest, 1) == 1); });
    while (!entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    assert(rejects([&] { limited.invoke_guest(y, echo_guest, 2); }));
    release_guest.store(true, std::memory_order_release);
    busy.join();
    assert(limited.invoke_guest(y, echo_guest, 3) == 3);
    for (const auto& h : {x, y}) {
        limited.begin_shutdown(h);
        limited.complete_shutdown(h);
        limited.destroy(h);
    }
}
