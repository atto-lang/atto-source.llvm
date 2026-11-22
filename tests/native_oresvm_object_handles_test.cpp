#include "oreslang/runtime/OresVM.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::IsolateMode;
using oreslang::runtime::OresVM;

// Never let -DNDEBUG compile away security assertions or their side effects.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "tenant object check failed: %s, line %d\\n", \
                     #condition, __LINE__); \
        return EXIT_FAILURE; \
    } \
} while (false)

template <class F>
bool rejects(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

static bool lifecycle_and_reuse() {
    OresVM vm;
    auto a = vm.create_isolate({"a", 48, 2, IsolateMode::TrustedInProcess, 64, 1024, 2});
    auto b = vm.create_isolate({"b", 48, 2, IsolateMode::TrustedInProcess, 64, 1024, 2});
    vm.start(a);
    vm.start(b);
    auto first = vm.allocate_object(a, 8);
    auto second = vm.allocate_object(a, 8);
    auto foreign = vm.allocate_object(b, 8);
    if (vm.snapshot(a).used_bytes != 16 || vm.snapshot(a).active_objects != 2 ||
        vm.snapshot(b).active_objects != 1) return false;
    if (!rejects([&] { vm.allocate_object(a, 1); })) return false; // descriptor cap
    vm.write_object_byte(a, first, 0, 0xa5);
    vm.write_object_byte(a, first, 7, 0x5a);
    vm.write_object_byte(a, second, 0, 0x33);
    vm.write_object_byte(b, foreign, 0, 0x77);
    if (vm.read_object_byte(a, first, 0) != 0xa5 ||
        vm.read_object_byte(a, second, 0) != 0x33 ||
        vm.read_object_byte(b, foreign, 0) != 0x77) return false;
    if (!rejects([&] { vm.read_object_byte(b, first, 0); }) ||
        !rejects([&] { vm.write_object_byte(a, foreign, 0, 3); }) ||
        !rejects([&] { vm.read_object_byte(a, first, 8); }) ||
        !rejects([&] { vm.write_object_byte(a, first, UINT32_MAX, 3); }))
        return false;

    vm.retire_object(a, first);
    if (vm.snapshot(a).active_objects != 1) return false;
    if (!rejects([&] { vm.read_object_byte(a, first, 0); }) ||
        !rejects([&] { vm.retire_object(a, first); })) return false;
    auto recycled = vm.allocate_object(a, 4);
    if (vm.snapshot(a).used_bytes != 16 || vm.snapshot(a).active_objects != 2)
        return false; // same physical descriptor/region reused
    for (std::uint32_t i = 0; i < 4; ++i)
        if (vm.read_object_byte(a, recycled, i) != 0) return false;
    if (!rejects([&] { vm.read_object_byte(a, recycled, 4); }) ||
        !rejects([&] { vm.write_object_byte(a, first, 0, 9); }))
        return false; // stale generation cannot alias the recycled slot
    vm.write_object_byte(a, recycled, 0, 0x11);
    if (vm.read_object_byte(a, second, 0) != 0x33 ||
        vm.read_object_byte(b, foreign, 0) != 0x77) return false;

    vm.begin_shutdown(a);
    if (!rejects([&] { vm.allocate_object(a, 1); }) ||
        !rejects([&] { vm.complete_shutdown(a); })) return false;
    if (vm.read_object_byte(a, recycled, 0) != 0x11) return false;
    vm.retire_object(a, recycled);
    vm.retire_object(a, second);
    if (vm.snapshot(a).active_objects != 0) return false;
    vm.complete_shutdown(a);
    if (!rejects([&] { vm.read_object_byte(a, recycled, 0); })) return false;
    vm.destroy(a);

    const auto anew = vm.create_isolate({"a", 48, 2});
    vm.start(anew);
    const auto live = vm.allocate_object(anew, 8);
    if (vm.read_object_byte(anew, live, 0) != 0 ||
        !rejects([&] { vm.read_object_byte(anew, recycled, 0); }))
        return false;
    OresVM wrong_manager;
    if (!rejects([&] { wrong_manager.read_object_byte(anew, live, 0); }))
        return false;
    vm.retire_object(anew, live);
    vm.begin_shutdown(anew);
    vm.complete_shutdown(anew);
    vm.destroy(anew);
    vm.retire_object(b, foreign);
    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);
    return true;
}

static bool rejected_admissions() {
    OresVM vm;
    if (!rejects([&] {
            vm.create_isolate({"bad", 16, 1, IsolateMode::TrustedInProcess, 64, 1, 0});
        }) ||
        !rejects([&] {
            vm.create_isolate({"bad", 16, 1, IsolateMode::TrustedInProcess, 64, 1, 65537});
        })) return false;
    const auto h = vm.create_isolate({"limits", 16, 1});
    vm.start(h);
    if (!rejects([&] { vm.allocate_object(h, 0); }) ||
        !rejects([&] { vm.allocate_object(h, 8, 3); }) ||
        !rejects([&] { vm.allocate_object(h, 8, 0); }) ||
        !rejects([&] { vm.allocate_object(h, 8, UINT32_C(1) << 31); }))
        return false;
    const auto one = vm.allocate_object(h, 8, 8);
    if (!rejects([&] { vm.allocate_object(h, 9, 8); })) return false;
    vm.retire_object(h, one);
    // A capacity-8 region cannot be reinterpreted as a larger object, and
    // failed admission must preserve the retired backing span.
    if (!rejects([&] { vm.allocate_object(h, 17); })) return false;
    const auto again = vm.allocate_object(h, 8, 8);
    if (vm.snapshot(h).used_bytes != 8 || vm.read_object_byte(h, again, 0) != 0)
        return false;
    vm.retire_object(h, again);
    vm.begin_shutdown(h);
    vm.complete_shutdown(h);
    vm.destroy(h);
    return true;
}

static bool concurrent_host_access() {
    OresVM vm;
    auto a = vm.create_isolate({"concurrent", 128, 2,
         IsolateMode::TrustedInProcess, 64, 1024, 8});
    vm.start(a);
    std::atomic<bool> healthy{true};
    std::vector<std::thread> carriers;
    for (int tid = 0; tid < 4; ++tid) {
        carriers.emplace_back([&, tid] {
            try {
                for (int iteration = 0; iteration < 200; ++iteration) {
                    const auto obj = vm.allocate_object(a, 8);
                    const auto value = static_cast<std::uint8_t>(tid + 10);
                    vm.write_object_byte(a, obj, 0, value);
                    if (vm.read_object_byte(a, obj, 0) != value)
                        healthy.store(false);
                    vm.retire_object(a, obj);
                    if (!rejects([&] { vm.read_object_byte(a, obj, 0); }))
                        healthy.store(false);
                }
            } catch (...) { healthy.store(false); }
        });
    }
    for (auto& carrier : carriers) carrier.join();
    if (!healthy.load() || vm.snapshot(a).active_objects != 0 ||
        vm.snapshot(a).used_bytes > 64) return false;
    vm.begin_shutdown(a);
    vm.complete_shutdown(a);
    vm.destroy(a);
    return true;
}

int main() {
    CHECK(lifecycle_and_reuse());
    CHECK(rejected_admissions());
    CHECK(concurrent_host_access());
    std::puts("PASS TenantVM host object generation, bounds, scrub, reuse and shutdown");
    return EXIT_SUCCESS;
}
