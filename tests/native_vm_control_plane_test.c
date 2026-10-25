#include "oreslang/runtime/NativeOresVM.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

enum { WRITERS = 4, NOTIFIES = 1000 };
typedef struct {
    atomic_int calls;
    atomic_int control_calls;
} State;
typedef struct {
    OresNativeVM* vm;
    int domain;
    atomic_int* failures;
} Producer;

static void require(int valid) { if (!valid) abort(); }
static void on_data(void* opaque, size_t slot, uint64_t msg) {
    State* s = (State*)opaque;
    require(slot == 0 && msg == 42);
    atomic_fetch_add(&s->calls, 1);
}
static void on_control(void* opaque, size_t slot, uint64_t msg) {
    State* s = (State*)opaque;
    require(slot == 0 && msg == 17);
    atomic_fetch_add(&s->control_calls, 1);
}
static void* flood(void* opaque) {
    Producer* p = (Producer*)opaque;
    for (int i = 0; i < NOTIFIES; ++i)
        if (!ores_native_vm_control_notify(
                p->vm, (enum OresNativeVMDomain)p->domain))
            atomic_fetch_add(p->failures, 1);
    return NULL;
}
int main(void) {
    State state[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};
    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = 1;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = 2;
        cfg.domains[i].on_message = on_data;
        cfg.domains[i].on_control = on_control;
        cfg.domains[i].context = &state[i];
    }
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    require(vm != NULL);
    require(ores_native_vm_control_pending(vm) == 0);
    require(ores_native_vm_control_pump(vm) == 0);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);
    require(!ores_native_vm_control_notify(
        vm, (enum OresNativeVMDomain)-1));
    require(!ores_native_vm_control_notify(
        vm, ORES_NATIVE_VM_DOMAIN_COUNT));
    require(ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN));
    require(ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN));
    require(ores_native_vm_control_notify(vm, ORES_NATIVE_VM_UNTRUSTED));
    require(ores_native_vm_control_pending(vm) ==
        ((1u << ORES_NATIVE_VM_MAIN) | (1u << ORES_NATIVE_VM_UNTRUSTED)));
    require(ores_native_vm_control_pump(vm) ==
        ((1u << ORES_NATIVE_VM_MAIN) | (1u << ORES_NATIVE_VM_UNTRUSTED)));
    require(ores_native_vm_control_pending(vm) == 0);
    require(ores_native_vm_control_pump(vm) == 0);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);
        require(atomic_load(&state[i].calls) == 0);
        require(atomic_load(&state[i].control_calls) == 0);
    }

    atomic_int failures = 0;
    Producer producers[WRITERS] = {0};
    pthread_t threads[WRITERS];
    for (int i = 0; i < WRITERS; ++i) {
        producers[i].vm = vm;
        producers[i].domain = i;
        producers[i].failures = &failures;
        require(pthread_create(&threads[i], NULL, flood, &producers[i]) == 0);
    }
    for (int i = 0; i < WRITERS; ++i)
        require(pthread_join(threads[i], NULL) == 0);
    require(atomic_load(&failures) == 0);
    require(ores_native_vm_control_pending(vm) == 15u);
    require(ores_native_vm_control_pump(vm) == 15u);
    require(ores_native_vm_control_pump(vm) == 0u);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);

    /* A real actor CONTROL delivery is independent of the host wake.
     * Its actor callback executes on the existing domain carrier pool. */
    OresNativeActorRef actor = ores_native_vm_spawn(
        vm, ORES_NATIVE_VM_MAIN, 0);
    require(actor.scheduler_domain != 0);
    require(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 0);
    require(ores_native_vm_publish_control(vm, ORES_NATIVE_VM_MAIN, actor, 17));
    require(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 1);
    require(ores_native_vm_control_pump(vm) == 0);
    ores_native_vm_retire(vm);
    require(!ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN));
    require(ores_native_vm_control_pending(vm) == 0);
    require(ores_native_vm_control_pump(vm) == 0);
    require(ores_native_vm_close(vm));
    require(atomic_load(&state[0].control_calls) == 1);
    require(atomic_load(&state[0].calls) == 0);
    return 0;
}
