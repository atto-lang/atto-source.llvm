#include "oreslang/runtime/NativePgBroadcast.hpp"
#include "oreslang/runtime/NativeOresVM.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using Registry = oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::native_pg_try_broadcast;
using Report = oreslang::runtime::NativePgBroadcastReport;
static void check(bool yes) { if (!yes) throw std::runtime_error("pg VM mailbox gate failed"); }
struct State {
    std::atomic<bool> blocked{false};
    std::atomic<bool> release{false};
    std::atomic<int> received{0};
};
static void deliver(void* context, size_t actor, uint64_t payload) {
    auto& state = *static_cast<State*>(context);
    if (actor == 0 && payload == 9) {
        state.blocked.store(true, std::memory_order_release);
        while (!state.release.load(std::memory_order_acquire))
            std::this_thread::yield();  // test harness only; never production guest
    }
    state.received.fetch_add(1, std::memory_order_relaxed);
}
int main() {
    State state;
    OresNativeVMConfig cfg{};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        auto& domain = cfg.domains[i];
        domain.actor_slots = 2;
        domain.mailbox_capacity = 2;
        domain.carrier_workers = 2;
        domain.on_message = deliver;
        domain.context = &state;
    }
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    check(vm != nullptr);
    const auto a = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 0);
    const auto b = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 1);
    check(a.scheduler_domain && b.scheduler_domain && a.generation && b.generation);
    Registry groups(100, 2, 2);
    auto join = groups.create_group(50, 2);
    check(groups.register_actor(101, a.generation));
    check(groups.register_actor(102, b.generation));
    check(groups.join(join, 101, a.generation));
    check(groups.join(join, 102, b.generation));

    // Bind each registry identity to an exact live VM ActorRef in trusted host
    // code. No numeric group ID or guest-supplied ref can mint a capability.
    auto send = [&](uint64_t id, uint64_t generation) noexcept {
        const OresNativeActorRef target = id == 101 ? a : (id == 102 ? b : OresNativeActorRef{});
        if (!target.scheduler_domain || target.generation != generation) return false;
        return ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, target, 42)
            == ORES_NATIVE_SEND_ACCEPTED;
    };
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, a, 9)
          == ORES_NATIVE_SEND_ACCEPTED);
    for (int spins = 0; spins < 1000000 && !state.blocked.load(); ++spins)
        std::this_thread::yield();
    check(state.blocked.load());
    OresNativeSendStatus fill = ORES_NATIVE_SEND_ACCEPTED;
    for (int i = 0; i < 8 && fill == ORES_NATIVE_SEND_ACCEPTED; ++i)
        fill = ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, a, 42);
    check(fill == ORES_NATIVE_SEND_MAILBOX_FULL);
    const auto partial = native_pg_try_broadcast(groups, 50, 2, send);
    check(partial.status == Report::Status::ok &&
          partial.snapshot_members == 2 && partial.rejected == 1 &&
          partial.accepted == 1 && partial.stale == 0);

    // STOP invalidates actor A mailbox admission; no stale pg membership may
    // override that authoritative decision. B remains independently live.
    check(ores_native_vm_stop_sealed(vm, ORES_NATIVE_VM_MAIN, a));
    const auto stopped = native_pg_try_broadcast(groups, 50, 2, send);
    check(stopped.rejected == 1 && stopped.accepted == 1);
    check(groups.retire_actor(101, a.generation));
    const auto only_b = native_pg_try_broadcast(groups, 50, 2, send);
    check(only_b.snapshot_members == 1 && only_b.accepted == 1);
    check(!groups.retire_actor(101, a.generation));
    const auto snapshot = groups.snapshot_group(50);
    check(snapshot && snapshot->members.size() == 1);
    check(groups.close_group(50));
    check(!groups.contains_snapshot_member(*snapshot, snapshot->members[0]));
    check(native_pg_try_broadcast(groups, 50, 2, send).status ==
          Report::Status::group_missing);
    const auto reopened = groups.create_group(50, 2);
    check(groups.join(reopened, 102, b.generation));
    check(!groups.contains_snapshot_member(*snapshot, snapshot->members[0]));

    // A second VM cannot accept another VM's domain-scoped ActorRef.
    OresNativeVM* foreign = ores_native_vm_open(&cfg);
    check(foreign != nullptr);
    check(ores_native_vm_try_send(foreign, ORES_NATIVE_VM_MAIN, b, 42)
          == ORES_NATIVE_SEND_INVALID_REF);
    check(ores_native_vm_close(foreign));

    state.release.store(true, std::memory_order_release);
    check(ores_native_vm_stop_sealed(vm, ORES_NATIVE_VM_MAIN, b));
    check(groups.retire_actor(102, b.generation));
    check(groups.close_group(50));
    check(ores_native_vm_close(vm));
}
