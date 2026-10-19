#define _POSIX_C_SOURCE 200809L
/* REAL native OresVM scheduler admission freeze. This test is NOT compiler-
 * lowered await/FFI pinning or authenticated AOT image publication. */
#include "oreslang/runtime/NativeOresVM.h"
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    atomic_int hold, entered, processed, new_processed, control, parallel;
    atomic_int wrong_order, version, attempted_own, attempted_cross;
    OresNativeVM* vm;
    OresNativeActorRef primary;
} State;
static void check(int condition) { if (!condition) abort(); }
static void deliver(void* raw, size_t actor, uint64_t n) {
    State* s = (State*)raw;
    if (actor == 1) {
        if (n != 99) abort();
        atomic_fetch_add(&s->parallel, 1);
        return;
    }
    if (actor != 0) abort();
    if (n == 0) {
        atomic_store(&s->entered, 1);
        while (atomic_load(&s->hold)) sched_yield();
    }
    /* The payload is also the deterministic FIFO ordinal. */
    if ((int)n != atomic_fetch_add(&s->processed, 1))
        atomic_fetch_add(&s->wrong_order, 1);
    if (atomic_load(&s->version) == 2)
        atomic_fetch_add(&s->new_processed, 1);
}
static void control(void* raw, size_t actor, uint64_t n) {
    State* s = (State*)raw;
    if (actor != 0 || n != 77) abort();
    atomic_fetch_add(&s->control, 1);
}
static void until(atomic_int* observed, int expected) {
    struct timespec pause = {0, 1000000};
    for (int attempts = 0; attempts < 3000; ++attempts) {
        if (atomic_load(observed) == expected) return;
        nanosleep(&pause, NULL);
    }
    abort();
}
static void wait_for_quiescence(OresNativeVM* vm,
                                enum OresNativeVMDomain domain,
                                OresNativeActorRef ref, uint64_t ticket) {
    struct timespec pause = {0, 1000000};
    for (int attempts = 0; attempts < 3000; ++attempts) {
        if (ores_native_vm_upgrade_quiesced(vm, domain, ref, ticket)) return;
        nanosleep(&pause, NULL);
    }
    abort();
}
int main(void) {
    State state = {0};
    atomic_store(&state.hold, 1);
    atomic_store(&state.version, 1);
    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = i == ORES_NATIVE_VM_MAIN ? 2 : 1;
        cfg.domains[i].mailbox_capacity = 8;
        cfg.domains[i].carrier_workers = i == ORES_NATIVE_VM_MAIN ? 2 : 1;
        cfg.domains[i].on_message = deliver;
        cfg.domains[i].on_control = control;
        cfg.domains[i].context = &state;
    }
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    check(vm != NULL);
    const enum OresNativeVMDomain main_domain = ORES_NATIVE_VM_MAIN;
    OresNativeActorRef a = ores_native_vm_spawn(vm, main_domain, 0);
    OresNativeActorRef other = ores_native_vm_spawn(vm, main_domain, 1);
    check(a.scheduler_domain && other.scheduler_domain);
    check(ores_native_vm_upgrade_epoch(vm, main_domain, a) == 1);
    check(ores_native_vm_try_send(vm, main_domain, a, 0)
          == ORES_NATIVE_SEND_ACCEPTED);
    until(&state.entered, 1);
    check(ores_native_vm_try_send(vm, main_domain, a, 1)
          == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_vm_try_send(vm, main_domain, a, 2)
          == ORES_NATIVE_SEND_ACCEPTED);

    uint64_t ticket = 0;
    check(!ores_native_vm_upgrade_freeze(vm, main_domain, a, 2, &ticket));
    check(!ores_native_vm_upgrade_freeze(vm, main_domain, other, 1, NULL));
    OresNativeActorRef forged = a;
    ++forged.generation;
    check(!ores_native_vm_upgrade_freeze(vm, main_domain, forged, 1, &ticket));
    check(ores_native_vm_upgrade_freeze(vm, main_domain, a, 1, &ticket));
    check(ticket == 1);
    uint64_t unused = 0;
    check(!ores_native_vm_upgrade_freeze(vm, main_domain, a, 1, &unused));
    check(ores_native_vm_try_send(vm, main_domain, a, 3)
          == ORES_NATIVE_SEND_UPGRADE_FROZEN);
    /* CONTROL has an independent reserved admission slot and keeps moving
     * while DATA is sealed. Another actor retains its own independent lane. */
    check(ores_native_vm_publish_control(vm, main_domain, a, 77));
    check(ores_native_vm_try_send(vm, main_domain, other, 99)
          == ORES_NATIVE_SEND_ACCEPTED);
    until(&state.parallel, 1);
    check(!ores_native_vm_upgrade_quiesced(vm, main_domain, a, ticket));
    check(!ores_native_vm_upgrade_finish(vm, main_domain, a, ticket, 1));
    check(!ores_native_vm_upgrade_finish(vm, main_domain, a, ticket + 1, 0));
    atomic_store(&state.hold, 0);
    until(&state.processed, 3);
    until(&state.control, 1);
    /* The last callback may still hold its execution claim after updating
     * user-visible counters. Wait for scheduler completion, not just count. */
    wait_for_quiescence(vm, main_domain, a, ticket);
    check(atomic_load(&state.new_processed) == 0);
    /* This atomic version switch stands in for the separately authorized
     * immutable-image CAS. Never expose this as a source-level upgrade API. */
    atomic_store(&state.version, 2);
    check(ores_native_vm_upgrade_finish(vm, main_domain, a, ticket, 1));
    check(ores_native_vm_upgrade_epoch(vm, main_domain, a) == 2);
    check(!ores_native_vm_upgrade_finish(vm, main_domain, a, ticket, 0));
    check(ores_native_vm_try_send(vm, main_domain, a, 3)
          == ORES_NATIVE_SEND_ACCEPTED);
    check(ores_native_vm_try_send(vm, main_domain, a, 4)
          == ORES_NATIVE_SEND_ACCEPTED);
    until(&state.processed, 5);
    until(&state.new_processed, 2);
    check(atomic_load(&state.wrong_order) == 0);

    uint64_t second = 0;
    check(ores_native_vm_upgrade_freeze(vm, main_domain, a, 2, &second));
    check(second > ticket);
    check(!ores_native_vm_upgrade_finish(vm, main_domain, a, ticket, 0));
    check(ores_native_vm_upgrade_finish(vm, main_domain, a, second, 0));
    check(ores_native_vm_upgrade_epoch(vm, main_domain, a) == 2);
    check(ores_native_vm_try_send(vm, main_domain, a, 5)
          == ORES_NATIVE_SEND_ACCEPTED);
    until(&state.processed, 6);

    /* Terminal STOP wins over any open freeze ticket; the old ticket must
     * never resurrect DATA admission for this actor incarnation. */
    uint64_t terminal = 0;
    check(ores_native_vm_upgrade_freeze(vm, main_domain, a, 2, &terminal));
    check(ores_native_vm_stop(vm, main_domain, a));
    check(!ores_native_vm_upgrade_finish(vm, main_domain, a, terminal, 0));
    check(!ores_native_vm_upgrade_finish(vm, main_domain, a, terminal, 1));
    check(ores_native_vm_try_send(vm, main_domain, a, 6)
          == ORES_NATIVE_SEND_TERMINATED);
    check(!ores_native_vm_upgrade_epoch(vm, main_domain, a));
    check(ores_native_vm_wait_done(vm, main_domain, a));
    check(ores_native_vm_stop(vm, main_domain, other));
    check(ores_native_vm_wait_done(vm, main_domain, other));
    check(ores_native_vm_close(vm));
    check(atomic_load(&state.new_processed) == 3);
    check(atomic_load(&state.wrong_order) == 0);
    puts("ACTUAL_NATIVE_VM_UPGRADE_INGRESS_PASS FIFO=6 control=1 epoch=2 stopped=terminal");
    return 0;
}
