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
    OresNativeActorRef child, grandchild;
    atomic_int holding_parent, release_parent, child_finished;
    atomic_int child_blocked, release_child;
    atomic_int control_seen, terminal_seen, grandchild_seen, parent_active, violations;
} State;
typedef struct {
    OresNativeVM* vm;
    OresNativeActorTerminalFailure failure;
    atomic_int* admitted;
} Sender;
typedef struct {
    OresNativeVM* vm;
    OresNativeActorRef supervisor;
    atomic_int* stopped;
} Stopper;

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
        atomic_store(&s->child_blocked, 1);
        while (!atomic_load(&s->release_child)) nap();
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
    if (parent == 1) {
        if (event.kind != ORES_NATIVE_CONTROL_STOPPED
            || event.child.scheduler_domain != s->grandchild.scheduler_domain
            || event.child.actor_slot != s->grandchild.actor_slot
            || event.child.generation != s->grandchild.generation)
            atomic_fetch_add(&s->violations, 1);
        atomic_fetch_add(&s->grandchild_seen, 1);
        return;
    }
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
static void* stop_supervisor(void* arg) {
    Stopper* s = (Stopper*)arg;
    if (ores_native_vm_stop(s->vm, ORES_NATIVE_VM_MAIN, s->supervisor))
        atomic_store(s->stopped, 1);
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
    s->grandchild = grandchild;
    actual = ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, grandchild);
    check(actual.actor_slot == 1 && actual.generation == s->child.generation);
    OresNativeActorRef unsupervised = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 3);
    check(unsupervised.scheduler_domain);
    check(!ores_native_vm_supervisor_of(vm, ORES_NATIVE_VM_MAIN, unsupervised).scheduler_domain);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 0);
    /* A parent with a live child cannot retire and discard its supervisor
     * obligation, even though both actors currently have zero carriers. */
    check(!ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, parent));
    check(!ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, s->child));
    /* Complete the grandchild's terminal notice before the child itself
     * exits; the child is its sole authoritative failure recipient. */
    check(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, grandchild));
    check(ores_native_vm_poll_done(vm, ORES_NATIVE_VM_MAIN, grandchild));
    OresNativeActorTerminalFailure grandchild_failure = {
        ORES_NATIVE_CONTROL_STOPPED, grandchild, 0, 7002
    };
    /* Scheduler DONE is not proof that host-owned finalizers completed.
     * Typed CONTROL must remain blocked until the trusted host receipts it. */
    check(!ores_native_vm_publish_supervised_failure(
        vm, ORES_NATIVE_VM_MAIN, grandchild_failure));
    check(ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, grandchild));
    check(!ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, grandchild));
    check(ores_native_vm_publish_supervised_failure(
        vm, ORES_NATIVE_VM_MAIN, grandchild_failure));
    until(&s->grandchild_seen, 1);
    check(!ores_native_vm_publish_supervised_failure(
        vm, ORES_NATIVE_VM_MAIN, grandchild_failure));

    OresNativeActorTerminalFailure failure = {
        ORES_NATIVE_CONTROL_PANIC, s->child, 19, 7001
    };
    /* Cannot finalize or report a live child, even if parent is idle. */
    check(!ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, s->child));
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, parent, 10)
        == ORES_NATIVE_SEND_ACCEPTED);
    until(&s->holding_parent, 1);
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, s->child, 20)
        == ORES_NATIVE_SEND_ACCEPTED);
    until(&s->child_blocked, 1);
    check(!ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, parent));
    check(!ores_native_vm_poll_done(vm, ORES_NATIVE_VM_MAIN, s->child));
    check(!ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, s->child));
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    atomic_store(&s->release_child, 1);
    until(&s->child_finished, 1);
    /* The stopped child is still on a carrier until its turn ends. The
     * official VM publication must only succeed after poll_done. */
    check(ores_native_vm_wait_done(vm, ORES_NATIVE_VM_MAIN, s->child));
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    check(!ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_SHARED, s->child));
    check(!ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, fake));
    check(!ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, unsupervised));
    check(ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, s->child));
    check(!ores_native_vm_confirm_supervised_finalized(
        vm, ORES_NATIVE_VM_MAIN, s->child));
    /* DONE plus finalized still owes terminal CONTROL. */
    check(!ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, parent));
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
    atomic_int parent_stopped = 0;
    Stopper stopper = {vm, parent, &parent_stopped};
    pthread_t p[2], stopper_thread;
    check(pthread_create(&p[0], NULL, publish, &sender) == 0);
    check(pthread_create(&stopper_thread, NULL, stop_supervisor, &stopper) == 0);
    check(pthread_create(&p[1], NULL, publish, &sender) == 0);
    for (int i=0; i<2; ++i) check(pthread_join(p[i], NULL) == 0);
    check(pthread_join(stopper_thread, NULL) == 0);
    check(atomic_load(&admitted) == 1);
    /* Either STOP lost the race and was correctly rejected, or it won
     * only after terminal CONTROL was already admitted. In both cases the
     * parent's handler must still execute exactly once. */
    if (!atomic_load(&parent_stopped))
        check(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, parent));
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    until(&s->terminal_seen, 1);
    check(ores_native_vm_stop(vm, ORES_NATIVE_VM_MAIN, unsupervised));
    check(ores_native_vm_stop(vm, ORES_NATIVE_VM_SHARED, foreign));
    ores_native_vm_retire(vm);
    check(!ores_native_vm_spawn_supervised(vm, ORES_NATIVE_VM_MAIN, 1, parent).scheduler_domain);
    check(!ores_native_vm_publish_supervised_failure(vm, ORES_NATIVE_VM_MAIN, failure));
    check(ores_native_vm_close(vm));
    check(atomic_load(&s->control_seen) == 1);
    check(atomic_load(&s->terminal_seen) == 1);
    check(atomic_load(&s->grandchild_seen) == 1);
    check(atomic_load(&s->violations) == 0);

    /* Domains without a typed terminal handler must reject supervised
     * construction BEFORE consuming the child actor slot. */
    cfg.domains[ORES_NATIVE_VM_MAIN].on_failure = NULL;
    OresNativeVM* no_handler = ores_native_vm_open(&cfg);
    check(no_handler != NULL);
    OresNativeActorRef root = ores_native_vm_spawn(no_handler, ORES_NATIVE_VM_MAIN, 0);
    check(root.scheduler_domain != 0);
    check(!ores_native_vm_spawn_supervised(
        no_handler, ORES_NATIVE_VM_MAIN, 1, root).scheduler_domain);
    /* Failed supervised construction did not consume the slot. */
    OresNativeActorRef free_slot = ores_native_vm_spawn(
        no_handler, ORES_NATIVE_VM_MAIN, 1);
    check(free_slot.scheduler_domain != 0);
    check(ores_native_vm_started_carriers(no_handler, ORES_NATIVE_VM_MAIN) == 0);
    check(ores_native_vm_stop(no_handler, ORES_NATIVE_VM_MAIN, free_slot));
    check(ores_native_vm_stop(no_handler, ORES_NATIVE_VM_MAIN, root));
    check(ores_native_vm_close(no_handler));
    printf("NATIVE_VM_TYPED_SUPERVISOR_PASS domains=4 once=1 quiescent=1\n");
    return 0;
}
