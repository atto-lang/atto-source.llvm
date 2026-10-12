#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    OresNativeVM* vm;
    OresNativeActorRef child;
    atomic_int holding_parent, release_parent, child_finished;
    atomic_int control_seen, terminal_seen, parent_active, violations;
} State;
typedef struct {
    OresNativeVM* vm;
    OresNativeActorTerminalFailure failure;
    atomic_int* admitted;
} Sender;

static void check(int ok) { if (!ok) abort(); }
static void nap(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static void until(atomic_int* ptr, int want) {
    for (int i=0; i<2000; ++i) {
        if (atomic_load(ptr) >= want) return;
        nap();
    }
    abort();
}
static void on_data(void* context, size_t actor, uint64_t value) {
    State* s = (State*)context;
    if (actor == 0) {
        check(value == 10);
        if (atomic_fetch_add(&s->parent_active, 1) != 0)
            atomic_fetch_add(&s->violations, 1);
        atomic_store(&s->holding_parent, 1);
        while (!atomic_load(&s->release_parent)) nap();
        atomic_fetch_sub(&s->parent_active, 1);
    } else if (actor == 1) {
        check(value == 20);
        check(ores_native_vm_stop(s->vm, ORES_NATIVE_VM_MAIN, s->child));
        atomic_store(&s->child_finished, 1);
    } else abort();
}
static void on_control(void* context, size_t actor, uint64_t value) {
    State* s = (State*)context;
    if (actor != 0 || value != 21 || atomic_load(&s->parent_active))
        atomic_fetch_add(&s->violations, 1);
    atomic_fetch_add(&s->control_seen, 1);
}
static void on_failure(void* context, size_t parent, OresNativeActorTerminalFailure event) {
    State* s = (State*)context;
    if (parent != 0 || event.kind != ORES_NATIVE_CONTROL_PANIC
        || event.failure_code != 19 || event.correlation != 7001
        || event.child.scheduler_domain != s->child.scheduler_domain
        || event.child.actor_slot != s->child.actor_slot
        || event.child.generation != s->child.generation
        || atomic_load(&s->parent_active))
        atomic_fetch_add(&s->violations, 1);
    atomic_fetch_add(&s->terminal_seen, 1);
}
static void* publish(void* arg) {
    Sender* p = (Sender*)arg;
    if (ores_native_vm_publish_supervised_failure(
            p->vm, ORES_NATIVE_VM_MAIN, p->failure))
        atomic_fetch_add(p->admitted, 1);
    return NULL;
}
int main(void) {
    State states[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};
    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = 4;
        cfg.domains[i].mailbox_capacity = 2;
        cfg.domains[i].carrier_workers = 3;
        cfg.domains[i].on_message = on_data;
        cfg.domains[i].on_control = on_control;
        cfg.domains[i].on_failure = on_failure;
        cfg.domains[i].context = &states[i];
    }
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    check(vm != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        states[i].vm = vm;
        check(ores_native_vm_started_carriers(vm, (enum OresNativeVMDomain)i) == 0);
    }
    OresNativeActorRef parent = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 0);
    OresNativeActorRef foreign = ores_native_vm_spawn(vm, ORES_NATIVE_VM_SHARED, 0);
    check(parent.scheduler_domain && foreign.scheduler_domain);
    check(!ores_native_vm_spawn_supervised(
        vm, ORES_NATIVE_VM_MAIN, 1, foreign).scheduler_domain);
    check(!ores_native_vm_spawn_supervised(
        vm, ORES_NATIVE_VM_MAIN, 0, parent).scheduler_domain);
    check(!ores_native_vm_spawn_supervised(
        vm, (enum OresNativeVMDomain)-1, 1, parent).scheduler_domain);
    State* s = &states[0];
    s->child = ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, 1, parent);
    check(s->child.scheduler_domain);
    check(!ores_native_vm_spawn_supervised(
        vm, ORES_NATIVE_VM_MAIN, 1, parent).scheduler_domain);
    OresNativeActorRef actual = ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, s->child);
    check(actual.scheduler_domain == parent.scheduler_domain
        && actual.actor_slot == parent.actor_slot && actual.generation == parent.generation);
    OresNativeActorRef fake = s->child;
    ++fake.generation;
    check(!ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, fake).scheduler_domain);
    check(!ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_SHARED, s->child).scheduler_domain);
    OresNativeActorRef grandchild = ores_native_vm_spawn_supervised(
        vm, ORES_NATIVE_VM_MAIN, 2, s->child);
    check(grandchild.scheduler_domain);
    actual = ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, grandchild);
    check(actual.actor_slot == 1 && actual.generation == s->child.generation);
    OresNativeActorRef unsupervised = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 3);
    check(unsupervised.scheduler_domain);
    check(!ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, unsupervised).scheduler_domain);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 0);

    OresNativeActorTerminalFailure failure = {
        ORES_NATIVE_CONTROL_PANIC, s->child, 19, 7001
    };
    /* Cannot report a live child, even if the parent is idle. */
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, parent, 10)
        == ORES_NATIVE_SEND_ACCEPTED);
    until(&s->holding_parent, 1);
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, s->child, 20)
        == ORES_NATIVE_SEND_ACCEPTED);
    until(&s->child_finished, 1);
    /* The stopped child is still on a carrier until its turn ends. The
     * official VM publication must only succeed after poll_done. */
    check(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s->child));
    check(ores_native_vm_publish_control(vm, ORES_NATIVE_VM_MAIN, parent, 21));
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_SHARED, failure));
    check(!ores_native_vm_publish_supervised_failure(vm, (enum OresNativeVMDomain)99, failure));
    failure.child = fake;
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    failure.child = unsupervised;
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    failure.child = s->child;
    atomic_store(&s->release_parent, 1);
    until(&s->control_seen, 1);
    /* Race duplicate publish attempts. Exactly one may claim this terminal
     * identity, irrespective of the parent carrier already consuming it. */
    atomic_int admitted = 0;
    Sender sender = {vm, failure, &admitted};
    pthread_t p[2];
    check(pthread_create(&p[0], NULL, publish, &sender) == 0);
    check(pthread_create(&p[1], NULL, publish, &sender) == 0);
    for (int i=0; i<2; ++i) check(pthread_join(p[i], NULL) == 0);
    check(atomic_load(&admitted) == 1);
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    until(&s->terminal_seen, 1);
    check(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, grandchild));
    check(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, unsupervised));
    check(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, parent));
    check(ores_native_vm_stop(vm, ORES_NATIVE_VM_SHARED, foreign));
    ores_native_vm_retire(vm);
    check(!ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, 1, parent).scheduler_domain);
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    check(ores_native_vm_close(vm));
    check(atomic_load(&s->control_seen) == 1);
    check(atomic_load(&s->terminal_seen) == 1);
    check(atomic_load(&s->violations) == 0);
    printf("NATIVE_VM_TYPED_SUPERVISOR_PASS domains=4 once=1 quiescent=1\n");
    return 0;
}
