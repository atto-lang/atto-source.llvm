#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Linux linker --wrap injects a failure into the real VM's first pthread
 * creation. This is NOT a model or a modified runtime production path. */
static atomic_int create_calls;
static atomic_int reject_once = 1;

extern int __real_pthread_create(
    pthread_t* thread, const pthread_attr_t* attrs,
    void* (*start)(void*), void* arg);

int __wrap_pthread_create(
    pthread_t* thread, const pthread_attr_t* attrs,
    void* (*start)(void*), void* arg) {
    atomic_fetch_add_explicit(&create_calls, 1, memory_order_relaxed);
    if (atomic_exchange_explicit(&reject_once, 0, memory_order_relaxed))
        return EAGAIN;
    return __real_pthread_create(thread, attrs, start, arg);
}

static void require(int ok) { if (!ok) abort(); }
static void noop(void* context, size_t slot, uint64_t message) {
    (void)context;
    (void)slot;
    (void)message;
}

int main(void) {
    OresNativeVMConfig config = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        config.domains[i].actor_slots = 1;
        config.domains[i].mailbox_capacity = 2;
        config.domains[i].carrier_workers = 1;
        config.domains[i].on_message = noop;
        config.domains[i].on_control = noop;
    }
    OresNativeVM* vm = ores_native_vm_open(&config);
    require(vm != NULL);
    require(atomic_load(&create_calls) == 0);
    require(ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN));
    require(!ores_native_vm_start_main_control_loop(vm)); /* EAGAIN */
    require(atomic_load(&create_calls) == 1);
    require(!ores_native_vm_main_control_loop_started(vm));
    require(ores_native_vm_control_pending(vm) == 1u);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);

    /* Failed pthread creation must not consume pending work or poison the
     * cold VM. Retrying from the host still starts exactly one master. */
    require(ores_native_vm_start_main_control_loop(vm));
    require(ores_native_vm_main_control_loop_started(vm));
    require(atomic_load(&create_calls) == 2);
    const struct timespec pause = {0, 1000000};
    int dispatched = 0;
    for (int n = 0; n < 3000; ++n) {
        if (ores_native_vm_control_wakes_dispatched(vm) >= 1) {
            dispatched = 1;
            break;
        }
        nanosleep(&pause, NULL);
    }
    require(dispatched);
    require(ores_native_vm_control_pending(vm) == 0);
    require(ores_native_vm_close(vm));

    /* The same injected EAGAIN during RT carrier preallocation is terminal
     * for the RT bootstrap. It must not leave a backdoor for a late CONTROL
     * pthread. The caller can still retire/close the failed cold VM. */
    vm = ores_native_vm_open(&config);
    require(vm != NULL);
    atomic_store_explicit(&reject_once, 1, memory_order_relaxed);
    require(!ores_native_vm_prepare_realtime(vm));
    require(atomic_load(&create_calls) == 3);
    require(!ores_native_vm_start_main_control_loop(vm));
    require(!ores_native_vm_main_control_loop_started(vm));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);
    require(ores_native_vm_close(vm));
    return 0;
}
