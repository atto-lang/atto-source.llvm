#include "oreslang/runtime/NativeEtsScalarSet.hpp"
#include "oreslang/runtime/NativePgBroadcast.hpp"
#include "oreslang/runtime/NativeOresVMNio.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <stdexcept>
#include <vector>

using Set = oreslang::runtime::NativeEtsScalarSet;
using Registry = oreslang::runtime::NativeActorMembershipRegistry;
using oreslang::runtime::native_pg_try_broadcast;

static void require(bool yes) {
    if (!yes) throw std::runtime_error("VM ETS/pg cold boot thread budget violation");
}
static void unexpected_actor(void*, std::size_t, std::uint64_t) {
    std::abort();
}
static std::vector<long> tids() {
    DIR* tasks = opendir("/proc/self/task");
    require(tasks != nullptr);
    std::vector<long> ids;
    struct dirent* e = nullptr;
    while ((e = readdir(tasks))) {
        char* end = nullptr;
        const long id = std::strtol(e->d_name, &end, 10);
        if (end != e->d_name && *end == '\0' && id > 0) ids.push_back(id);
    }
    require(closedir(tasks) == 0);
    std::sort(ids.begin(), ids.end());
    return ids;
}
int main() {
    const auto before = tids();
    OresNativeVMConfig cfg{};
    for (int domain = 0; domain < ORES_NATIVE_VM_DOMAIN_COUNT; ++domain) {
        auto& d = cfg.domains[domain];
        d.actor_slots = 2;
        d.mailbox_capacity = 8;
        d.carrier_workers = 2;
        d.on_message = unexpected_actor;
    }
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    require(vm != nullptr);
    require(ores_native_vm_enable_nio(vm, 4, 0, 1));
    require(ores_native_vm_nio_started(vm) == 0);

    Set table({10, 20, 30, 1}, Set::Protection::protected_table, 16);
    auto handle = table.handle();
    require(table.insert(handle, {10, 20, 30, 1}, 1, 9) == Set::Status::ok);
    require(table.lookup(handle, {10, 20, 31, 1}, 1).value == 9);
    Registry groups(10, 3, 2);
    auto cap = groups.create_group(5, 4);
    require(groups.register_actor(30, 1));
    require(groups.register_actor(31, 1));
    require(groups.join(cap, 30, 1));
    require(groups.join(cap, 31, 1));
    auto report = native_pg_try_broadcast(
        groups, 5, 4,
        [](std::uint64_t, std::uint64_t) noexcept {
            return false; // no authentic mailbox bridge in this host-only slice
        });
    require(report.snapshot_members == 2 && report.rejected == 2);
    require(ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN));
    require(ores_native_vm_control_pump(vm) == 1u);
    require(ores_native_vm_nio_started(vm) == 0);
    for (int d = 0; d < ORES_NATIVE_VM_DOMAIN_COUNT; ++d)
        require(ores_native_vm_started_carriers(
            vm, static_cast<OresNativeVMDomain>(d)) == 0);
    // Prove absence of unrelated bootstrap threads (e.g. hidden table,
    // group or master pthreads), beyond merely checking carrier counts.
    require(tids() == before);
    require(groups.retire_actor(30, 1));
    require(groups.retire_actor(31, 1));
    require(groups.close_group(5));
    require(table.retire_owner({10, 20, 30, 1}));
    require(ores_native_vm_close(vm));
    require(tids() == before);
}
