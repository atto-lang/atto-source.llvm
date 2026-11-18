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

// Deliberately permissive fake callbacks: the generated guest must refuse
// malformed context metadata BEFORE invoking any callback at all.
static std::atomic<int> fake_callback_calls{0};
extern "C" int fake_load(void*, std::uint32_t, std::int64_t* out) {
    ++fake_callback_calls;
    if (out) *out = 123;
    return 1;
}
extern "C" int fake_store(void*, std::uint32_t, std::int64_t) {
    ++fake_callback_calls;
    return 1;
}

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
    std::int64_t fake_data = 88;
    std::int64_t fake_out = 777;
    OresIsolateContext malformed{
        ORES_ISOLATE_ABI_VERSION, ORES_ISOLATE_I64_SLOTS, 0,
        &fake_data, &fake_load, &fake_store};
    CHECK(ores_guest_entry(&malformed, 1, &fake_out) == ORES_GUEST_BAD_CONTEXT);
    CHECK(fake_callback_calls.load() == 0 && fake_out == 777);
    malformed.generation = 1;
    malformed.abi_version = 0;
    CHECK(ores_guest_entry(&malformed, 1, &fake_out) == ORES_GUEST_BAD_CONTEXT);
    malformed.abi_version = ORES_ISOLATE_ABI_VERSION;
    malformed.slot_count = 0;
    CHECK(ores_guest_entry(&malformed, 1, &fake_out) == ORES_GUEST_BAD_CONTEXT);
    CHECK(fake_callback_calls.load() == 0 && fake_out == 777);

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
