#include "oreslang/runtime/NativeOresVM.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

static atomic_int received[ORES_NATIVE_VM_DOMAIN_COUNT];
static void require(int yes) { if (!yes) abort(); }
static void deliver(void* ctx, size_t actor, uint64_t value) {
    int domain = *(int*)ctx;
    require(actor == 0 && value == 42);
    atomic_fetch_add_explicit(&received[domain], 1, memory_order_relaxed);
}

int main(void) {
    OresNativeVMConfig cfg = {0};
    int ids[ORES_NATIVE_VM_DOMAIN_COUNT];
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        ids[i] = i;
        cfg.domains[i].actor_slots = 1;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = 3;
        cfg.domains[i].on_message = deliver;
        cfg.domains[i].context = &ids[i];
    }

    /* Normal throughput open/spawn starts no workers until runnable work. */
    OresNativeVM* lazy = ores_native_vm_open(&cfg);
    require(lazy != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain domain = (enum OresNativeVMDomain)i;
        require(ores_native_vm_started_carriers(lazy, domain) == 0);
        require(ores_native_vm_spawn(lazy, domain, 0).scheduler_domain != 0);
        require(ores_native_vm_started_carriers(lazy, domain) == 0);
    }
    require(ores_native_vm_close(lazy));

    /* RT open performs potentially blocking pthread creation only at host
     * bootstrap; thereafter all per-domain physical capacity is provisioned. */
    OresNativeVM* vm = ores_native_vm_open_realtime(&cfg);
    require(vm != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain domain = (enum OresNativeVMDomain)i;
        require(ores_native_vm_workers(vm, domain) == 3);
        require(ores_native_vm_started_carriers(vm, domain) == 3);
        require(!ores_native_vm_prepare_realtime(vm)); /* cannot bootstrap twice */
        OresNativeActorRef actor = ores_native_vm_spawn(vm, domain, 0);
        require(actor.scheduler_domain != 0);
        require(ores_native_vm_try_send(vm, domain, actor, 42) == ORES_NATIVE_SEND_ACCEPTED);
        require(ores_native_vm_started_carriers(vm, domain) == 3);
    }

    /* External host fence and teardown. Already admitted work must drain. */
    require(ores_native_vm_close(vm));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(atomic_load_explicit(&received[i], memory_order_relaxed) == 1);
    return 0;
}
