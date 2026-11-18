// Trusted-host physical actor-turn / object provenance test.
// Not a native guest sandbox and not compiler-backed .atto execution.
#include "oreslang/runtime/OresVM.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <vector>

using oreslang::runtime::IsolateMode;
using oreslang::runtime::OresVM;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "actor turn/object gate failed: %s at %d\\n", \
                     #condition, __LINE__); \
        return EXIT_FAILURE; \
    } \
} while (false)

template<typename Fn>
bool denies(Fn&& action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}

extern "C" std::int32_t trusted_guest_probe(
    OresIsolateContext* ctx, std::int64_t input, std::int64_t* out) {
    if (!ctx || !out || ctx->abi_version != ORES_ISOLATE_ABI_VERSION)
        return ORES_GUEST_BAD_CONTEXT;
    *out = input;
    return ORES_GUEST_OK;
}

int main() {
    OresVM vm;
    auto a = vm.create_isolate({"actor_a", 256, 8});
    auto b = vm.create_isolate({"actor_b", 256, 8});
    vm.start(a);
    vm.start(b);
    const auto a1 = vm.register_actor(a, 1);
    const auto a2 = vm.register_actor(a, 2);
    const auto b1 = vm.register_actor(b, 1);

    // Same native carrier cannot hold two active tenant-domain turns.
    // Actor ID equality is NOT what protects this boundary: different
    // tenant roots, actor IDs and OresVM hosts must all be denied.
    OresVM other_root;
    const auto other = other_root.create_isolate({"independent_root", 128, 2});
    other_root.start(other);
    const auto other_actor = other_root.register_actor(other, 7);
    const auto first_turn = vm.enter_actor_turn(a, a1);
    CHECK(denies([&] { (void)vm.enter_actor_turn(b, b1); }));
    CHECK(denies([&] { (void)other_root.enter_actor_turn(other, other_actor); }));
    // Plain tenant guests have no actor-turn provenance, so they must not
    // re-enter while this physical carrier owns a live actor turn.
    CHECK(denies([&] { (void)vm.invoke_guest(a, trusted_guest_probe, 1); }));
    CHECK(denies([&] { (void)vm.invoke_guest(b, trusted_guest_probe, 2); }));
    CHECK(denies([&] { (void)other_root.invoke_guest(other, trusted_guest_probe, 3); }));
    CHECK(denies([&] { (void)vm.enter_actor_turn(a, a1); }));
    CHECK(denies([&] { (void)vm.enter_actor_turn(a, a2); }));
    const auto obj = vm.allocate_actor_object(a, a1, first_turn, 8);
    vm.write_actor_object_byte(a, a1, first_turn, obj, 0, 71);
    CHECK(vm.read_actor_object_byte(a, a1, first_turn, obj, 0) == 71);
    CHECK(denies([&] { (void)vm.read_object_byte(a, obj, 0); }));
    CHECK(denies([&] { vm.write_object_byte(a, obj, 0, 42); }));
    CHECK(denies([&] { vm.retire_object(a, obj); }));
    CHECK(denies([&] { (void)vm.read_actor_object_byte(a, a2, first_turn, obj, 0); }));
    CHECK(denies([&] { (void)vm.read_actor_object_byte(b, b1, first_turn, obj, 0); }));
    CHECK(denies([&] { (void)vm.allocate_actor_object(a, a2, first_turn, 1); }));
    CHECK(denies([&] { vm.retire_actor(a, a1); }));
    CHECK(denies([&] { vm.read_actor_object_byte(a, a1, first_turn, obj, 8); }));

    // An authentic live turn is still not portable to a different pthread.
    std::atomic<bool> cross_thread_denied{false};
    std::thread foreign([&] {
        cross_thread_denied.store(
            denies([&] { (void)vm.read_actor_object_byte(a, a1, first_turn, obj, 0); }) &&
            denies([&] { vm.write_actor_object_byte(a, a1, first_turn, obj, 0, 12); }) &&
            denies([&] { vm.leave_actor_turn(a, a1, first_turn); }));
    });
    foreign.join();
    CHECK(cross_thread_denied.load());
    CHECK(vm.read_actor_object_byte(a, a1, first_turn, obj, 0) == 71);
    vm.leave_actor_turn(a, a1, first_turn);
    // Leaving the owning turn clears global carrier admission. A different
    // root may then run on this same pthread, but not concurrently with
    // any other tenant turn in the first root.
    const auto other_turn = other_root.enter_actor_turn(other, other_actor);
    CHECK(denies([&] { (void)vm.enter_actor_turn(b, b1); }));
    other_root.leave_actor_turn(other, other_actor, other_turn);
    // After releasing all actor-owned capabilities, legacy trusted scalar
    // guests can run on this carrier (not an untrusted actor sandbox).
    CHECK(vm.invoke_guest(b, trusted_guest_probe, 17) == 17);
    other_root.retire_actor(other, other_actor);
    other_root.begin_shutdown(other);
    other_root.complete_shutdown(other);
    other_root.destroy(other);
    CHECK(denies([&] { vm.leave_actor_turn(a, a1, first_turn); }));
    CHECK(denies([&] { (void)vm.read_actor_object_byte(a, a1, first_turn, obj, 0); }));
    CHECK(denies([&] { vm.retire_actor(a, a1); })); // live owned object

    const auto resumed = vm.enter_actor_turn(a, a1);
    CHECK(vm.read_actor_object_byte(a, a1, resumed, obj, 0) == 71);
    CHECK(denies([&] { vm.write_actor_object_byte(a, a1, first_turn, obj, 0, 0); }));
    vm.retire_actor_object(a, a1, resumed, obj);
    CHECK(denies([&] { (void)vm.read_actor_object_byte(a, a1, resumed, obj, 0); }));
    const auto reused = vm.allocate_actor_object(a, a1, resumed, 4);
    CHECK(vm.snapshot(a).used_bytes == 8); // same physical slot
    CHECK(vm.read_actor_object_byte(a, a1, resumed, reused, 0) == 0);
    CHECK(denies([&] { vm.write_actor_object_byte(a, a1, resumed, obj, 0, 99); }));
    CHECK(denies([&] { vm.read_actor_object_byte(a, a1, resumed, reused, 4); }));
    vm.write_actor_object_byte(a, a1, resumed, reused, 0, 55);
    vm.leave_actor_turn(a, a1, resumed);

    // Quiesce seals new ordinary work. A separate cleanup-only turn may
    // retire owned bytes but cannot allocate, read, or mutate them.
    vm.begin_shutdown(a);
    CHECK(denies([&] { (void)vm.enter_actor_turn(a, a1); }));
    CHECK(denies([&] { vm.retire_actor(a, a1); }));
    CHECK(denies([&] { vm.complete_shutdown(a); }));
    const auto cleanup = vm.enter_actor_cleanup_turn(a, a1);
    CHECK(denies([&] { (void)vm.allocate_actor_object(a, a1, cleanup, 1); }));
    CHECK(denies([&] { (void)vm.read_actor_object_byte(a, a1, cleanup, reused, 0); }));
    CHECK(denies([&] { vm.write_actor_object_byte(a, a1, cleanup, reused, 0, 99); }));
    vm.retire_actor_object(a, a1, cleanup, reused);
    vm.leave_actor_turn(a, a1, cleanup);
    vm.retire_actor(a, a1);
    vm.retire_actor(a, a2);
    vm.complete_shutdown(a);
    vm.destroy(a);

    // Matching actor ID and slot in a recreated tenant never authorizes
    // old actor or object tickets, even when the VM reuses the tenant name.
    const auto anew = vm.create_isolate({"actor_a", 256, 8});
    vm.start(anew);
    const auto a1_new = vm.register_actor(anew, 1);
    CHECK(denies([&] { (void)vm.enter_actor_turn(anew, a1); }));
    const auto new_turn = vm.enter_actor_turn(anew, a1_new);
    CHECK(denies([&] { (void)vm.read_actor_object_byte(anew, a1_new, new_turn, obj, 0); }));
    const auto fresh = vm.allocate_actor_object(anew, a1_new, new_turn, 8);
    CHECK(vm.read_actor_object_byte(anew, a1_new, new_turn, fresh, 0) == 0);
    vm.retire_actor_object(anew, a1_new, new_turn, fresh);
    vm.leave_actor_turn(anew, a1_new, new_turn);
    vm.retire_actor(anew, a1_new);
    vm.begin_shutdown(anew);
    vm.complete_shutdown(anew);
    vm.destroy(anew);

    // A second tenant may run the same actor numeric ID independently.
    const auto bturn = vm.enter_actor_turn(b, b1);
    const auto bobj = vm.allocate_actor_object(b, b1, bturn, 8);
    vm.write_actor_object_byte(b, b1, bturn, bobj, 0, 8);
    CHECK(vm.read_actor_object_byte(b, b1, bturn, bobj, 0) == 8);
    vm.retire_actor_object(b, b1, bturn, bobj);
    vm.leave_actor_turn(b, b1, bturn);
    vm.retire_actor(b, b1);
    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);

    // Independent actors on distinct carriers can use the same tenant heap
    // without a global single-actor turn; each token stays thread-bound.
    OresVM parallel_vm;
    const auto parallel = parallel_vm.create_isolate({"parallel", 512, 4});
    parallel_vm.start(parallel);
    std::vector<OresVM::ActorTicket> actors;
    for (std::uint64_t id = 1; id <= 4; ++id)
        actors.emplace_back(parallel_vm.register_actor(parallel, id));
    std::atomic<bool> healthy{true};
    std::vector<std::thread> carriers;
    for (int i = 0; i < 4; ++i) {
        carriers.emplace_back([&, i] {
            try {
                for (int iteration = 0; iteration < 100; ++iteration) {
                    const auto turn = parallel_vm.enter_actor_turn(parallel, actors[i]);
                    const auto object = parallel_vm.allocate_actor_object(
                        parallel, actors[i], turn, 8);
                    parallel_vm.write_actor_object_byte(
                        parallel, actors[i], turn, object, 0,
                        static_cast<std::uint8_t>(i + 1));
                    if (parallel_vm.read_actor_object_byte(
                            parallel, actors[i], turn, object, 0) != i + 1)
                        healthy.store(false);
                    parallel_vm.retire_actor_object(parallel, actors[i], turn, object);
                    parallel_vm.leave_actor_turn(parallel, actors[i], turn);
                }
            } catch (...) { healthy.store(false); }
        });
    }
    for (auto& carrier : carriers) carrier.join();
    CHECK(healthy.load());
    CHECK(parallel_vm.snapshot(parallel).active_objects == 0);
    for (const auto& actor : actors) parallel_vm.retire_actor(parallel, actor);
    parallel_vm.begin_shutdown(parallel);
    parallel_vm.complete_shutdown(parallel);
    parallel_vm.destroy(parallel);
    std::puts("PASS TenantVM actor-turn ownership, cleanup, stale/foreign and carrier gates");
    return EXIT_SUCCESS;
}
