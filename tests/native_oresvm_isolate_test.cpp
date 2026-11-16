#include "oreslang/runtime/OresVM.hpp"

#include <cassert>
#include <type_traits>

// The tenant VM is an actual C++ owner type, but only OresVM may create it.
static_assert(std::is_same_v<oreslang::runtime::TenantVM,
                             oreslang::runtime::IsolateVM>);
static_assert(!std::is_default_constructible<oreslang::runtime::TenantVM>::value,
              "canonical TenantVM cannot be constructed by guest code");
static_assert(!std::is_default_constructible<oreslang::runtime::IsolateVM>::value,
              "guest code cannot directly create a tenant IsolateVM");
static_assert(!std::is_copy_constructible<oreslang::runtime::IsolateVM>::value,
              "tenant ownership must never be copied");
static_assert(!std::is_move_constructible<oreslang::runtime::IsolateVM>::value,
              "tenant-owned live state must not be moved");
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using oreslang::runtime::IsolateMode;
using oreslang::runtime::IsolatePhase;
using oreslang::runtime::OresVM;

template<class Fn>
bool rejects(Fn&& fn) {
    try { fn(); } catch (const std::exception&) { return true; }
    return false;
}

int main() {
    OresVM vm;
    assert(rejects([&] { vm.create_isolate({"guest", 64, 2,
                                            IsolateMode::SandboxedProcess}); }));
    assert(rejects([&] { vm.create_isolate({"", 64, 2}); }));
    // Cross-backend fail-closed identity admission: previous native host
    // accepted path separators, colon, Unicode and dot-only components.
    // Rejections must occur before generation, heap or tenant quota charges.
    const std::vector<std::string> invalid_ids{
        ".", "..", "../escape", "tenant/other", "tenant\\other",
        "tenant:other", "tenant other", std::string("bad\\0tenant", 10),
        "t\\xC3\\xA9nant", std::string(129, 'x')
    };
    for (const auto& id : invalid_ids) {
        assert(rejects([&] { vm.create_isolate({id, 64, 2}); }));
    }
    auto boundary = vm.create_isolate({std::string(128, 'x'), 16, 1});
    vm.start(boundary);
    assert(vm.snapshot(boundary).phase == IsolatePhase::Running);
    vm.begin_shutdown(boundary);
    vm.complete_shutdown(boundary);
    vm.destroy(boundary);
    OresVM::TenantVMHandle a = vm.create_tenant_vm({"tenant-a", 32, 2});
    auto b = vm.create_isolate({"tenant-b", 32, 2});
    assert(rejects([&] { vm.create_isolate({"tenant-a", 32, 2}); }));
    assert(rejects([&] { vm.allocate(a, 1); }));
    vm.start(a);
    vm.start(b);
    assert(rejects([&] { vm.start(a); }));
    auto a1 = vm.allocate(a, 1);
    auto b1 = vm.allocate(b, 1);
    assert(a1.offset() == 0 && b1.offset() == 0);
    vm.write_byte(a, a1, 0, 17);
    vm.write_byte(b, b1, 0, 29);
    assert(vm.read_byte(a, a1, 0) == 17);
    assert(vm.read_byte(b, b1, 0) == 29);
    assert(rejects([&] { vm.read_byte(a, b1, 0); }));
    assert(rejects([&] { vm.write_byte(b, a1, 0, 9); }));
    assert(rejects([&] { vm.read_byte(a, a1, 1); }));
    assert(rejects([&] { vm.allocate(a, 1, 3); }));
    assert(rejects([&] { vm.allocate(a, 0); }));
    auto aligned = vm.allocate(a, 1, 8);
    assert(aligned.offset() == 8);
    assert(vm.snapshot(a).used_bytes == 9);
    assert(rejects([&] { vm.allocate(a, 24); }));
    assert(vm.snapshot(a).used_bytes == 9);

    auto actor_a = vm.register_actor(a, 1);
    auto actor_a2 = vm.register_actor(a, 2);
    auto actor_b = vm.register_actor(b, 1);
    vm.retire_actor(b, actor_b);
    const auto actor_b_new = vm.register_actor(b, 1);
    assert(rejects([&] { vm.retire_actor(b, actor_b); }));
    assert(rejects([&] { vm.register_actor(a, 1); }));
    assert(rejects([&] { vm.register_actor(a, 3); }));
    assert(rejects([&] { vm.retire_actor(a, actor_b); }));
    assert(vm.snapshot(a).active_actors == 2);
    vm.begin_shutdown(a);
    assert(rejects([&] { vm.allocate(a, 1); }));
    assert(rejects([&] { vm.register_actor(a, 3); }));
    assert(vm.read_byte(a, a1, 0) == 17);
    assert(rejects([&] { vm.complete_shutdown(a); }));
    vm.retire_actor(a, actor_a);
    assert(rejects([&] { vm.register_actor(a, 1); }));
    vm.retire_actor(a, actor_a2);
    assert(rejects([&] { vm.retire_actor(a, actor_a2); }));
    vm.complete_shutdown(a);
    assert(vm.snapshot(a).phase == IsolatePhase::Stopped);
    assert(rejects([&] { vm.read_byte(a, a1, 0); }));
    vm.destroy(a);
    assert(rejects([&] { vm.snapshot(a); }));
    auto anew = vm.create_isolate({"tenant-a", 16, 1});
    vm.start(anew);
    assert(vm.snapshot(anew).generation != vm.snapshot(b).generation);
    assert(rejects([&] { vm.read_byte(anew, a1, 0); }));
    OresVM another_vm;
    assert(rejects([&] { another_vm.snapshot(anew); }));

    // Concurrent allocations across independent tenants never expose a raw
    // pointer and are serialized through OresVM admission.
    auto c = vm.create_isolate({"tenant-c", 4096, 1});
    auto d = vm.create_isolate({"tenant-d", 4096, 1});
    vm.start(c);
    vm.start(d);
    auto writer = [&](const OresVM::IsolateHandle& h, std::uint8_t value) {
        for (int i = 0; i < 256; ++i) {
            const auto slot = vm.allocate(h, 1);
            vm.write_byte(h, slot, 0, value);
            assert(vm.read_byte(h, slot, 0) == value);
        }
    };
    std::thread t1(writer, std::cref(c), std::uint8_t{33});
    std::thread t2(writer, std::cref(d), std::uint8_t{44});
    t1.join();
    t2.join();
    assert(vm.snapshot(c).used_bytes == 256);
    assert(vm.snapshot(d).used_bytes == 256);

    for (const auto& h : {b, anew, c, d}) {
        if (vm.snapshot(h).active_actors != 0)
            vm.retire_actor(h, actor_b_new);
        vm.begin_shutdown(h);
        vm.complete_shutdown(h);
        vm.destroy(h);
    }
}
