#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    OresNativeVM* vm;
    OresNativeActorRef parent, first, second;
    atomic_int parent_started, release_parent, active, max_active, completed;
    int trace[4];
    int trace_count;
} State;

static void require(int ok) { if (!ok) abort(); }
static void tick(void) {
    struct timespec t = {0, 1000000};
    nanosleep(&t, NULL);
}
static void wait_for(atomic_int* value, int n) {
    for (int i = 0; i < 5000; ++i) {
        if (atomic_load(value) >= n) return;
        tick();
    }
    abort();
}
static void enter(State* s, size_t actor) {
    require(actor == 0);
    int active = atomic_fetch_add(&s->active, 1) + 1;
    int prior = atomic_load(&s->max_active);
    while (active > prior && !atomic_compare_exchange_weak(
               &s->max_active, &prior, active)) { }
    require(active == 1);
}
static void leave(State* s) {
    require(atomic_fetch_sub(&s->active, 1) == 1);
}
static void append(State* s, int payload) {
    require(s->trace_count < 4);
    s->trace[s->trace_count++] = payload;
    atomic_store(&s->completed, s->trace_count);
}
static void on_data(void* opaque, size_t actor, uint64_t payload) {
    State* s = (State*)opaque;
    require(actor == 0 && payload == 11);
    enter(s, actor);
    append(s, 11);
    atomic_store(&s->parent_started, 1);
    while (!atomic_load(&s->release_parent)) tick();
    leave(s);
}
static void on_control(void* opaque, size_t actor, uint64_t payload) {
    State* s = (State*)opaque;
    require(actor == 0 && payload == 77);
    enter(s, actor);
    append(s, 77);
    leave(s);
}
static void on_failure(void* opaque, size_t actor,
                       OresNativeActorTerminalFailure failure) {
    State* s = (State*)opaque;
    require(actor == 0 && failure.kind == ORES_NATIVE_CONTROL_PANIC);
    enter(s, actor);
    if (failure.child.actor_slot == s->first.actor_slot) {
        require(failure.child.scheduler_domain == s->first.scheduler_domain);
        require(failure.child.generation == s->first.generation);
        require(failure.correlation == 101);
        append(s, 101);
    } else {
        require(failure.child.actor_slot == s->second.actor_slot);
        require(failure.child.scheduler_domain == s->second.scheduler_domain);
        require(failure.child.generation == s->second.generation);
        require(failure.correlation == 102);
        append(s, 102);
    }
    leave(s);
}
int main(void) {
    State s = {0};
    OresNativeVMConfig cfg = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = 3;
        cfg.domains[i].mailbox_capacity = 1;
        cfg.domains[i].carrier_workers = 2;
        cfg.domains[i].on_message = on_data;
        cfg.domains[i].on_control = on_control;
        cfg.domains[i].on_failure = on_failure;
        cfg.domains[i].context = &s;
    }
    s.vm = ores_native_vm_open(&cfg);
    require(s.vm != NULL);
    s.parent = ores_native_vm_spawn(s.vm, ORES_NATIVE_VM_MAIN, 0);
    s.first = ores_native_vm_spawn_supervised(
        s.vm, ORES_NATIVE_VM_MAIN, 1, s.parent);
    s.second = ores_native_vm_spawn_supervised(
        s.vm, ORES_NATIVE_VM_MAIN, 2, s.parent);
    require(s.parent.generation && s.first.generation && s.second.generation);

    require(ores_native_vm_try_send(s.vm, ORES_NATIVE_VM_MAIN, s.parent, 11)
            == ORES_NATIVE_SEND_ACCEPTED);
    wait_for(&s.parent_started, 1);
    require(ores_native_vm_try_send(s.vm, ORES_NATIVE_VM_MAIN, s.parent, 12)
            == ORES_NATIVE_SEND_MAILBOX_FULL);
    require(ores_native_vm_stop(s.vm, ORES_NATIVE_VM_MAIN, s.first));
    require(ores_native_vm_stop(s.vm, ORES_NATIVE_VM_MAIN, s.second));
    require(ores_native_vm_wait_done(s.vm, ORES_NATIVE_VM_MAIN, s.first));
    require(ores_native_vm_wait_done(s.vm, ORES_NATIVE_VM_MAIN, s.second));
    require(ores_native_vm_confirm_supervised_finalized(
        s.vm, ORES_NATIVE_VM_MAIN, s.first));
    require(ores_native_vm_confirm_supervised_finalized(
        s.vm, ORES_NATIVE_VM_MAIN, s.second));

    require(ores_native_vm_publish_control(s.vm, ORES_NATIVE_VM_MAIN, s.parent, 77));
    OresNativeActorTerminalFailure first = {
        ORES_NATIVE_CONTROL_PANIC, s.first, 99, 101
    };
    OresNativeActorTerminalFailure second = {
        ORES_NATIVE_CONTROL_PANIC, s.second, 99, 102
    };
    require(ores_native_vm_publish_supervised_failure(
        s.vm, ORES_NATIVE_VM_MAIN, first));
    require(ores_native_vm_publish_supervised_failure(
        s.vm, ORES_NATIVE_VM_MAIN, second));
    require(!ores_native_vm_publish_supervised_failure(
        s.vm, ORES_NATIVE_VM_MAIN, first));
    /* Terminal receipts are already accepted even though parent is busy. */
    require(!ores_native_vm_stop(s.vm, ORES_NATIVE_VM_MAIN, s.parent)
            || s.trace_count == 1); /* A parent may stop after child admission. */

    atomic_store(&s.release_parent, 1);
    wait_for(&s.completed, 4);
    require(s.trace_count == 4);
    require(s.trace[0] == 11 && s.trace[1] == 77
            && s.trace[2] == 101 && s.trace[3] == 102);
    require(atomic_load(&s.max_active) == 1);
    ores_native_vm_retire(s.vm);
    require(ores_native_vm_close(s.vm));
    puts("NATIVE_VM_MULTI_CHILD_CONTROL_PASS distinct=2 outstanding=0");
    return 0;
}
