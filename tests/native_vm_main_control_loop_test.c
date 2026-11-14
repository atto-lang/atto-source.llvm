#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

enum { WRITERS = 4, NOTIFICATIONS = 300 };
typedef struct {
    atomic_int guest_data;
    atomic_int actor_control;
} Counters;
typedef struct {
    OresNativeVM* vm;
    enum OresNativeVMDomain domain;
    atomic_int* rejected;
} Writer;

static void require(int condition) { if (!condition) abort(); }
static void on_data(void* opaque, size_t slot, uint64_t payload) {
    Counters* counters = (Counters*)opaque;
    require(slot == 0 && payload == 42);
    atomic_fetch_add(&counters->guest_data, 1);
}
static void on_control(void* opaque, size_t slot, uint64_t payload) {
    Counters* counters = (Counters*)opaque;
    require(slot == 0 && payload == 17);
    atomic_fetch_add(&counters->actor_control, 1);
}
static OresNativeVMConfig make_config(Counters counters[ORES_NATIVE_VM_DOMAIN_COUNT]) {
    OresNativeVMConfig config = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        config.domains[i].actor_slots = 1;
        config.domains[i].mailbox_capacity = 4;
        config.domains[i].carrier_workers = 2;
        config.domains[i].on_message = on_data;
        config.domains[i].on_control = on_control;
        config.domains[i].context = &counters[i];
    }
    return config;
}
static void await_dispatches(OresNativeVM* vm, uint64_t minimum) {
    const struct timespec tick = {0, 1000000}; /* host test only */
    for (int attempt = 0; attempt < 3000; ++attempt) {
        if (ores_native_vm_control_wakes_dispatched(vm) >= minimum) return;
        nanosleep(&tick, NULL);
    }
    abort();
}
static void* flood(void* opaque) {
    Writer* writer = (Writer*)opaque;
    for (int i = 0; i < NOTIFICATIONS; ++i)
        if (!ores_native_vm_control_notify(writer->vm, writer->domain))
            atomic_fetch_add(writer->rejected, 1);
    return NULL;
}
int main(void) {
    Counters counters[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};
    OresNativeVMConfig config = make_config(counters);
    require(!ores_native_vm_start_main_control_loop(NULL));
    require(!ores_native_vm_main_control_loop_started(NULL));
    require(ores_native_vm_control_wakes_dispatched(NULL) == 0);

    OresNativeVM* vm = ores_native_vm_open(&config);
    require(vm != NULL);
    require(!ores_native_vm_main_control_loop_started(vm));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);

    /* An early wake is not lost when the host explicitly prestarts CONTROL. */
    require(ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN));
    require(ores_native_vm_start_main_control_loop(vm));
    require(ores_native_vm_start_main_control_loop(vm)); /* idempotent while cold */
    require(ores_native_vm_main_control_loop_started(vm));
    await_dispatches(vm, 1);
    for (int i = 1; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        require(ores_native_vm_control_notify(vm, (enum OresNativeVMDomain)i));
        await_dispatches(vm, (uint64_t)i + 1);
    }
    require(ores_native_vm_control_pending(vm) == 0);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);
        require(atomic_load(&counters[i].guest_data) == 0);
        require(atomic_load(&counters[i].actor_control) == 0);
    }

    atomic_int rejected = 0;
    Writer writers[WRITERS];
    pthread_t threads[WRITERS];
    for (int i = 0; i < WRITERS; ++i) {
        writers[i] = (Writer){vm, (enum OresNativeVMDomain)i, &rejected};
        require(pthread_create(&threads[i], NULL, flood, &writers[i]) == 0);
    }
    for (int i = 0; i < WRITERS; ++i)
        require(pthread_join(threads[i], NULL) == 0);
    require(atomic_load(&rejected) == 0);
    require(ores_native_vm_control_wakes_dispatched(vm) >= 4);
    /* Coalescing a thousand host signals still starts no actor carriers. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 0);

    /* Real actor CONTROL messages are independent and use the MAIN carrier,
     * never the VM's CONTROL coordinator pthread. */
    OresNativeActorRef root = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 0);
    require(root.scheduler_domain != 0);
    require(!ores_native_vm_start_main_control_loop(vm)); /* too late to bootstrap */
    require(ores_native_vm_publish_control(vm, ORES_NATIVE_VM_MAIN, root, 17));
    require(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 1);
    ores_native_vm_retire(vm);
    require(!ores_native_vm_control_notify(vm, ORES_NATIVE_VM_MAIN));
    require(!ores_native_vm_start_main_control_loop(vm));
    require(ores_native_vm_close(vm)); /* joins CONTROL before scheduler free */
    require(atomic_load(&counters[0].actor_control) == 1);
    require(atomic_load(&counters[0].guest_data) == 0);

    /* A competing cold NIO bootstrap excludes main-loop thread creation. */
    vm = ores_native_vm_open(&config);
    require(vm != NULL);
    require(ores_native_vm_nio_begin_bootstrap(vm));
    require(!ores_native_vm_start_main_control_loop(vm));
    ores_native_vm_nio_cancel_bootstrap(vm);
    require(ores_native_vm_start_main_control_loop(vm));
    require(ores_native_vm_close(vm));

    /* Real-time preallocation is a one-way OS-thread creation barrier.
     * Neither a successful RT bootstrap nor an RT-open VM can subsequently
     * provision the main CONTROL pthread on a guest/hot execution path. */
    vm = ores_native_vm_open(&config);
    require(vm != NULL);
    require(ores_native_vm_prepare_realtime(vm));
    require(!ores_native_vm_start_main_control_loop(vm));
    require(!ores_native_vm_main_control_loop_started(vm));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        require(ores_native_vm_started_carriers(
            vm, (enum OresNativeVMDomain)i) == 2);
    require(ores_native_vm_close(vm));

    vm = ores_native_vm_open_realtime(&config);
    require(vm != NULL);
    require(!ores_native_vm_start_main_control_loop(vm));
    require(!ores_native_vm_main_control_loop_started(vm));
    require(ores_native_vm_close(vm));

    /* If both are required, start CONTROL while cold, before RT prestart.
     * A later, repeated CONTROL start must still reject the RT transition. */
    vm = ores_native_vm_open(&config);
    require(vm != NULL);
    require(ores_native_vm_start_main_control_loop(vm));
    require(ores_native_vm_prepare_realtime(vm));
    require(!ores_native_vm_start_main_control_loop(vm));
    require(ores_native_vm_main_control_loop_started(vm));
    require(ores_native_vm_close(vm));
    return 0;
}
