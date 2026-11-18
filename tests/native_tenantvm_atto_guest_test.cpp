// Real oreslang-llvmc .atto -> LLVM IR -> native guest. Not a host model.
// The trusted OresVM owns tenant-global state and all invocation tickets.
#include "oreslang/runtime/OresVM.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <vector>

using oreslang::runtime::IsolateMode;
using oreslang::runtime::IsolatePhase;
using oreslang::runtime::OresVM;

extern "C" std::int32_t ores_guest_entry(
    OresIsolateContext*, std::int64_t, std::int64_t*);

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "TenantVM .atto guest FAIL: %s line %d\\n", \
                     #condition, __LINE__); \
        return EXIT_FAILURE; \
    } \
} while (false)

template<typename Fn>
bool denies(Fn&& operation) {
    try { operation(); }
    catch (const std::exception&) { return true; }
    return false;
}

int main() {
    // Distinct tenant heaps and source-private module state, one immutable
    // compiler-emitted function pointer shared by two tenants.
    OresVM vm;
    const auto a = vm.create_isolate({"tenant_a", 256, 1});
    const auto b = vm.create_isolate({"tenant_b", 256, 1});
    vm.start(a);
    vm.start(b);
    CHECK(vm.invoke_guest(a, ores_guest_entry, -3) == -3);
    CHECK(vm.invoke_guest(a, ores_guest_entry, 5) == 2);
    CHECK(vm.invoke_guest(b, ores_guest_entry, 100) == 100);
    CHECK(vm.invoke_guest_result(a, ores_guest_entry, 0).status == ORES_GUEST_OK);
    CHECK(vm.invoke_guest(b, ores_guest_entry, 0) == 100);

    // A real source-generated callback must preserve tenant-private globals
    // under concurrent carriers, without a single shared global counter.
    std::atomic<bool> healthy{true};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            try {
                for (int n = 0; n < 100; ++n)
                    (void)vm.invoke_guest(a, ores_guest_entry, 1);
            } catch (...) { healthy.store(false); }
        });
    }
    for (auto& worker : workers) worker.join();
    CHECK(healthy.load());
    CHECK(vm.invoke_guest(a, ores_guest_entry, 0) == 402);
    CHECK(vm.invoke_guest(b, ores_guest_entry, 0) == 100);

    // Checked overflow is recoverable; it cannot publish a half-written
    // global or turn a neighbor's state into a wrapped scalar.
    const auto overflow = vm.invoke_guest_result(
        b, ores_guest_entry, INT64_MAX);
    CHECK(overflow.status == ORES_GUEST_OVERFLOW && overflow.value == 0);
    CHECK(vm.invoke_guest(b, ores_guest_entry, 0) == 100);
    const auto negative = vm.invoke_guest_result(
        a, ores_guest_entry, INT64_MIN);
    CHECK(negative.status == ORES_GUEST_OVERFLOW && negative.value == 0);
    CHECK(vm.invoke_guest(a, ores_guest_entry, 0) == 402);

    // Checked allocation handles remain strictly tenant-scoped, even if
    // callers pass the wrong host handle from a separate native pthread.
    const auto mem_a = vm.allocate(a, 1);
    const auto mem_b = vm.allocate(b, 1);
    vm.write_byte(a, mem_a, 0, 23);
    vm.write_byte(b, mem_b, 0, 29);
    CHECK(denies([&] { (void)vm.read_byte(a, mem_b, 0); }));
    CHECK(denies([&] { vm.write_byte(b, mem_a, 0, 31); }));
    CHECK(vm.read_byte(a, mem_a, 0) == 23);
    CHECK(vm.read_byte(b, mem_b, 0) == 29);

    vm.begin_shutdown(a);
    CHECK(denies([&] { (void)vm.invoke_guest(a, ores_guest_entry, 1); }));
    CHECK(vm.invoke_guest(b, ores_guest_entry, 1) == 101);
    vm.complete_shutdown(a);
    vm.destroy(a);
    const auto new_a = vm.create_isolate({"tenant_a", 256, 1});
    vm.start(new_a);
    CHECK(vm.invoke_guest(new_a, ores_guest_entry, 0) == 0);
    CHECK(denies([&] { (void)vm.snapshot(a); }));
    CHECK(denies([&] { (void)vm.read_byte(new_a, mem_a, 0); }));
    CHECK(vm.snapshot(new_a).phase == IsolatePhase::Running);
    vm.begin_shutdown(new_a);
    vm.complete_shutdown(new_a);
    vm.destroy(new_a);
    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);
    std::puts("PASS real .atto -> LLVM -> native TenantVM scalar guest AOT and isolation");
    return EXIT_SUCCESS;
}
