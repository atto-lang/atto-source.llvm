// Cross-layer integration: production oreslang-llvmc .atto -> LLVM IR -> native
// OresVM guest entry AND trusted-host actor-owned object/physical carrier gate.
// This is NOT actor-aware guest lowering or a sandbox for hostile native code.
#include "oreslang/runtime/OresVM.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using oreslang::runtime::OresVM;

extern "C" std::int32_t ores_guest_entry(
    OresIsolateContext*, std::int64_t, std::int64_t*);

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "integrated .atto TenantVM/actor gate FAIL %s:%d: %s\n", \
                     __FILE__, __LINE__, #condition); \
        return EXIT_FAILURE; \
    } \
} while (false)

template<class F>
bool denies_authority(F&& fn) {
    // Unexpected std::bad_alloc, runtime_error or guest failure is not a
    // successful provenance rejection. These are security negative tests.
    try { fn(); }
    catch (const std::logic_error&) { return true; }
    return false;
}

struct ReentryProbe {
    OresVM* vm;
    OresVM::IsolateHandle tenant;
    OresVM::ActorTicket actor;
    std::uint32_t denied = 0;
};
// The ABI callback executes synchronously on the calling thread. The trusted
// test driver alone supplies this host-owned pointer; no untrusted guest is
// ever given an OresVM object or an actor-turn ticket.
static ReentryProbe* current_probe = nullptr;

extern "C" std::int32_t attempt_actor_from_scalar_guest(
    OresIsolateContext* ctx, std::int64_t value, std::int64_t* out) {
    if (!ctx || !out || !current_probe ||
        ctx->abi_version != ORES_ISOLATE_ABI_VERSION)
        return ORES_GUEST_BAD_CONTEXT;
    if (!denies_authority([&] {
            const auto t = current_probe->vm->enter_actor_turn(
                current_probe->tenant, current_probe->actor);
            current_probe->vm->leave_actor_turn(
                current_probe->tenant, current_probe->actor, t);
        }))
        return ORES_GUEST_FAILED;
    ++current_probe->denied;
    *out = value;
    return ORES_GUEST_OK;
}

// A held unscoped scalar guest rejects foreign-carrier actor admission.
static std::atomic<bool> scalar_guest_entered{false};
static std::atomic<bool> finish_scalar_guest{false};
extern "C" std::int32_t hold_scalar_guest(
    OresIsolateContext* ctx, std::int64_t value, std::int64_t* out) {
    if (!ctx || !out || ctx->abi_version != ORES_ISOLATE_ABI_VERSION)
        return ORES_GUEST_BAD_CONTEXT;
    scalar_guest_entered.store(true, std::memory_order_release);
    while (!finish_scalar_guest.load(std::memory_order_acquire))
        std::this_thread::yield();
    *out = value;
    return ORES_GUEST_OK;
}

int main() {
    OresVM vm;
    const auto a = vm.create_isolate({"integrated_a", 256, 3});
    const auto b = vm.create_isolate({"integrated_b", 256, 3});
    vm.start(a);
    vm.start(b);
    const auto a1 = vm.register_actor(a, 1);
    const auto a2 = vm.register_actor(a, 2);
    const auto b1 = vm.register_actor(b, 1);

    // Executed generated LLVM guest is a trusted, tenant-global scalar
    // transition, not an actor-scoped function. Two tenants remain separate.
    CHECK(vm.invoke_guest(a, ores_guest_entry, 11) == 11);
    CHECK(vm.invoke_guest(b, ores_guest_entry, 37) == 37);

    const auto turn = vm.enter_actor_turn(a, a1);
    const auto owned = vm.allocate_actor_object(a, a1, turn, 16);
    vm.write_actor_object_byte(a, a1, turn, owned, 0, 0xA5);
    CHECK(vm.read_actor_object_byte(a, a1, turn, owned, 0) == 0xA5);

    // The same carrier cannot turn its scoped actor authority into a
    // generic source-compiled guest invocation, even in another tenant.
    CHECK(denies_authority([&] {
        (void)vm.invoke_guest(a, ores_guest_entry, 100);
    }));
    CHECK(denies_authority([&] {
        (void)vm.invoke_guest(b, ores_guest_entry, 200);
    }));
    CHECK(denies_authority([&] {
        (void)vm.read_actor_object_byte(a, a2, turn, owned, 0);
    }));
    CHECK(denies_authority([&] {
        (void)vm.read_actor_object_byte(b, b1, turn, owned, 0);
    }));
    CHECK(vm.read_actor_object_byte(a, a1, turn, owned, 0) == 0xA5);
    // Other pthreads are also denied generic guest admission to the
    // actor's own tenant, while separate tenants retain independent
    // scheduling and storage, even on that other carrier thread.
    std::atomic<bool> cross_carrier_denied{false};
    std::atomic<bool> separate_tenant_ok{false};
    std::thread other_carrier([&] {
        cross_carrier_denied.store(denies_authority([&] {
            (void)vm.invoke_guest(a, ores_guest_entry, 100);
        }));
        try {
            separate_tenant_ok.store(vm.invoke_guest(b, ores_guest_entry, 2) == 39);
        } catch (...) { separate_tenant_ok.store(false); }
    });
    other_carrier.join();
    CHECK(cross_carrier_denied.load() && separate_tenant_ok.load());
    vm.retire_actor_object(a, a1, turn, owned);
    vm.leave_actor_turn(a, a1, turn);

    // Failed nested source guests must never publish half-written state.
    CHECK(vm.invoke_guest(a, ores_guest_entry, 0) == 11);
    CHECK(vm.invoke_guest(b, ores_guest_entry, 0) == 39);
    CHECK(vm.invoke_guest(a, ores_guest_entry, 4) == 15);
    CHECK(vm.invoke_guest(b, ores_guest_entry, -7) == 32);

    // Reverse direction: an unscoped tenant-global guest callback may not
    // acquire actor-turn authority, even for an otherwise live actor.
    ReentryProbe probe{&vm, a, a2, 0};
    current_probe = &probe;
    CHECK(vm.invoke_guest(a, attempt_actor_from_scalar_guest, 5) == 5);
    CHECK(probe.denied == 1);
    current_probe = nullptr;

    // Cross-carrier reverse direction: a scalar guest already executing in
    // tenant A also prevents actor admission from a distinct pthread.
    // Both the observation and release are bounded to avoid a stuck worker
    // and to keep a failure from aborting with a joinable std::thread.
    scalar_guest_entered.store(false);
    finish_scalar_guest.store(false);
    std::atomic<bool> scalar_completed{false};
    std::thread scalar_carrier([&] {
        try {
            scalar_completed.store(vm.invoke_guest(a, hold_scalar_guest, 7) == 7);
        } catch (...) { scalar_completed.store(false); }
    });
    for (int spin = 0; spin < 1000000 &&
            !scalar_guest_entered.load(std::memory_order_acquire); ++spin)
        std::this_thread::yield();
    const bool active_guest_observed =
        scalar_guest_entered.load(std::memory_order_acquire);
    const bool actor_admission_denied = active_guest_observed &&
        denies_authority([&] {
            const auto forbidden = vm.enter_actor_turn(a, a2);
            vm.leave_actor_turn(a, a2, forbidden);
        });
    finish_scalar_guest.store(true, std::memory_order_release);
    scalar_carrier.join();
    CHECK(active_guest_observed && actor_admission_denied &&
          scalar_completed.load());

    const auto new_turn = vm.enter_actor_turn(a, a2);
    const auto new_obj = vm.allocate_actor_object(a, a2, new_turn, 8);
    CHECK(vm.read_actor_object_byte(a, a2, new_turn, new_obj, 0) == 0);
    vm.retire_actor_object(a, a2, new_turn, new_obj);
    vm.leave_actor_turn(a, a2, new_turn);

    // Ownership/revocation survives a later tenant phase transition.
    vm.begin_shutdown(a);
    CHECK(denies_authority([&] {
        (void)vm.invoke_guest(a, ores_guest_entry, 1);
    }));
    vm.retire_actor(a, a1);
    vm.retire_actor(a, a2);
    vm.complete_shutdown(a);
    vm.destroy(a);
    CHECK(vm.invoke_guest(b, ores_guest_entry, 0) == 32);
    vm.retire_actor(b, b1);
    vm.begin_shutdown(b);
    vm.complete_shutdown(b);
    vm.destroy(b);

    std::puts("PASS REAL_ATTO_LLVM_AOT_PLUS_ACTOR_TURN_HOST_PROVENANCE");
    return EXIT_SUCCESS;
}
