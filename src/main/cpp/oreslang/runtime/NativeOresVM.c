#include "oreslang/runtime/NativeOresVM.h"
#include <stdlib.h>

struct OresNativeVM {
    OresNativeActorScheduler* schedulers[ORES_NATIVE_VM_DOMAIN_COUNT];
    size_t workers[ORES_NATIVE_VM_DOMAIN_COUNT];
};

static int valid_domain(enum OresNativeVMDomain domain) {
    return domain >= ORES_NATIVE_VM_MAIN && domain < ORES_NATIVE_VM_DOMAIN_COUNT;
}

OresNativeVM* ores_native_vm_open(const OresNativeVMConfig* config) {
    if (!config) return NULL;
    OresNativeVM* vm = (OresNativeVM*)calloc(1, sizeof(*vm));
    if (!vm) return NULL;
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        const OresNativeVMDomainConfig* p = &config->domains[i];
        size_t workers = p->carrier_workers ? p->carrier_workers : 5;
        /* Validate all host callbacks and slot/capacity budgets before use.
         * NativeActorScheduler additionally enforces its allocation limits. */
        if (!p->on_message || !p->actor_slots || !p->mailbox_capacity
            || workers > 128) goto failed;
        vm->schedulers[i] = ores_native_actor_scheduler_open_control(
            p->actor_slots, p->mailbox_capacity, workers,
            p->on_message, p->on_control, p->context);
        if (!vm->schedulers[i]) goto failed;
        vm->workers[i] = workers;
    }
    return vm;
failed:
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (vm->schedulers[i])
            ores_native_actor_scheduler_retire(vm->schedulers[i]);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (vm->schedulers[i]) {
            if (!ores_native_actor_scheduler_drain(vm->schedulers[i])) abort();
            if (!ores_native_actor_scheduler_try_destroy(vm->schedulers[i])) abort();
        }
    free(vm);
    return NULL;
}

size_t ores_native_vm_started_carriers(
    OresNativeVM* vm, enum OresNativeVMDomain domain) {
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_started_carriers(vm->schedulers[domain]) : 0;
}
size_t ores_native_vm_workers(const OresNativeVM* vm, enum OresNativeVMDomain domain) {
    return vm && valid_domain(domain) ? vm->workers[domain] : 0;
}

OresNativeActorRef ores_native_vm_spawn(
    OresNativeVM* vm, enum OresNativeVMDomain domain, size_t actor_slot) {
    OresNativeActorRef invalid = {0, 0, 0};
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_spawn(vm->schedulers[domain], actor_slot) : invalid;
}

OresNativeSendStatus ores_native_vm_try_send(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef ref, uint64_t payload) {
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_try_send(vm->schedulers[domain], ref, payload)
        : ORES_NATIVE_SEND_INVALID_REF;
}

int ores_native_vm_stop(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_stop(vm->schedulers[domain], ref);
}

int ores_native_vm_publish_control(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef parent, uint64_t payload) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_publish_control(
            vm->schedulers[domain], parent, payload);
}

void ores_native_vm_retire(OresNativeVM* vm) {
    if (!vm) return;
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        ores_native_actor_scheduler_retire(vm->schedulers[i]);
}

int ores_native_vm_close(OresNativeVM* vm) {
    if (!vm) return 0;
    ores_native_vm_retire(vm);
    /* Reject synchronous teardown from *any* owned carrier before waiting on
     * any domain. Without this preflight, a callback in UNTRUSTED can block
     * forever draining MAIN while MAIN waits for its callback to finish. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (ores_native_actor_scheduler_is_current_carrier(vm->schedulers[i]))
            return 0;
    /* No callback can synchronously join or destroy its own scheduler. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (!ores_native_actor_scheduler_drain(vm->schedulers[i])) return 0;
    /* Caller-owned API/producer fence is required before memory reclamation. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (!ores_native_actor_scheduler_try_destroy(vm->schedulers[i])) abort();
    free(vm);
    return 1;
}
