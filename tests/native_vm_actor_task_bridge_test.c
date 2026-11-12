#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"

#include <assert.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* This test executes the REAL NativeOresVM -> NativeActorScheduler TASK lane.
 * It is trusted host execution, NOT compiled .atto task/await guest evidence. */
typedef struct {
    OresNativeVM* vm;
    enum OresNativeVMDomain domain;
    OresNativeActorRef actor;
    OresNativeActorRef foreign;
    atomic_int active;
    atomic_int running;
    atomic_int release;
    atomic_int data_entered;
    atomic_int task_runs;
    atomic_int task_cancels;
    atomic_int nested;
} Domain;

static void actor_turn(Domain* d, size_t index) {
    assert(index == 0 && d->vm && d->actor.scheduler_domain != 0);
    OresNativeExecutionContext physical = {0};
    assert(ores_native_vm_current_execution_context(
        d->vm, d->domain, &physical));
    assert(physical.id.runtime_domain == d->actor.scheduler_domain);
    assert(physical.allocation.kind == ORES_ALLOCATION_DOMAIN_ACTOR);
    assert(physical.allocation.owner == index);
    assert(physical.allocation.generation == d->actor.generation);
}

static void data_callback(void* opaque, size_t index, uint64_t payload) {
    Domain* d = (Domain*)opaque;
    actor_turn(d, index);
    assert(atomic_fetch_add(&d->active, 1) == 0);
    assert(payload == (uint64_t)d->domain + 1u);
    if (d->domain == ORES_NATIVE_VM_UNTRUSTED) {
        /* Unconfigured domains may NEVER route to a foreign Task pool. */
        assert(ores_native_vm_try_schedule_task(
            d->vm, d->domain, d->actor, 1) == ORES_NATIVE_TASK_NOT_CONFIGURED);
    } else {
        /* A physical actor turn cannot schedule into another VM domain,
         * even when slot and incarnation numbers happen to be identical. */
        assert(ores_native_vm_try_schedule_task(
            d->vm, d->domain, d->foreign, 55)
            == ORES_NATIVE_TASK_FOREIGN_CARRIER);
        assert(ores_native_vm_try_schedule_task(
            d->vm, d->domain, d->actor, 0) == ORES_NATIVE_TASK_INVALID_ACTOR);
        assert(ores_native_vm_try_schedule_task(
            d->vm, d->domain, d->actor, (uint64_t)payload)
            == ORES_NATIVE_TASK_ACCEPTED);
        if (d->domain == ORES_NATIVE_VM_SHARED) {
            /* STOP seals while DATA owns the physical lease. The queued
             * task is cleanup-only; it must not execute guest on_task. */
            atomic_store(&d->data_entered, 1);
            while (!atomic_load(&d->release)) sched_yield();
        }
    }
    assert(atomic_fetch_sub(&d->active, 1) == 1);
}

static void task_callback(void* opaque, size_t index, uint64_t handle) {
    Domain* d = (Domain*)opaque;
    actor_turn(d, index);
    assert(atomic_fetch_add(&d->active, 1) == 0);
    if (d->domain == ORES_NATIVE_VM_MAIN) {
        assert(handle == 1);
        atomic_store(&d->running, 1);
        while (!atomic_load(&d->release)) sched_yield();
    } else {
        assert(d->domain == ORES_NATIVE_VM_TRUSTED_PRIVATE);
        assert(handle == 3 || handle == 33);
        if (handle == 3) {
            assert(ores_native_vm_try_schedule_task(
                d->vm, d->domain, d->actor, 33) == ORES_NATIVE_TASK_ACCEPTED);
            atomic_fetch_add(&d->nested, 1);
        }
    }
    atomic_fetch_add(&d->task_runs, 1);
    assert(atomic_fetch_sub(&d->active, 1) == 1);
}

static void task_cancel(void* opaque, size_t index, uint64_t handle) {
    Domain* d = (Domain*)opaque;
    actor_turn(d, index);
    assert(d->domain == ORES_NATIVE_VM_SHARED && handle == 2);
    assert(atomic_fetch_add(&d->active, 1) == 0);
    assert(ores_native_vm_try_schedule_task(
        d->vm, d->domain, d->actor, 99) != ORES_NATIVE_TASK_ACCEPTED);
    atomic_fetch_add(&d->task_cancels, 1);
    assert(atomic_fetch_sub(&d->active, 1) == 1);
}

static void until(atomic_int* flag, int wanted) {
    struct timespec start;
    assert(timespec_get(&start, TIME_UTC) == TIME_UTC);
    for (;;) {
        if (atomic_load(flag) == wanted) return;
        struct timespec now;
        assert(timespec_get(&now, TIME_UTC) == TIME_UTC);
        assert(now.tv_sec - start.tv_sec < 15);
        sched_yield();
    }
}

int main(void) {
    OresNativeVMConfig cfg = {0};
    OresNativeVMTaskConfig tasks = {0};
    Domain d[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};

    assert(!ores_native_vm_open_with_tasks(&cfg, NULL));
    assert(!ores_native_vm_open_with_wakes_and_tasks(&cfg, NULL, &tasks));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        d[i].domain = (enum OresNativeVMDomain)i;
        cfg.domains[i].actor_slots = 1;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].carrier_workers = 1;
        cfg.domains[i].on_message = data_callback;
        cfg.domains[i].context = &d[i];
        if (i != ORES_NATIVE_VM_UNTRUSTED) {
            tasks.on_task[i] = task_callback;
            tasks.on_task_cancel[i] = task_cancel;
        }
    }
    OresNativeVMTaskConfig invalid = tasks;
    invalid.on_task_cancel[ORES_NATIVE_VM_MAIN] = NULL;
    assert(!ores_native_vm_open_with_tasks(&cfg, &invalid));

    OresNativeVM* vm = ores_native_vm_open_with_tasks(&cfg, &tasks);
    assert(vm);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        d[i].vm = vm;
        d[i].actor = ores_native_vm_spawn(vm, d[i].domain, 0);
        assert(d[i].actor.scheduler_domain != 0);
    }
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        d[i].foreign = d[(i + 1) % ORES_NATIVE_VM_DOMAIN_COUNT].actor;

    /* A root caller cannot enter any actor TASK lane. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        const OresNativeTaskAdmissionStatus status =
            ores_native_vm_try_schedule_task(vm, d[i].domain, d[i].actor, 1);
        assert(status != ORES_NATIVE_TASK_ACCEPTED);
    }
    assert(ores_native_vm_try_schedule_task(
        NULL, ORES_NATIVE_VM_MAIN, d[0].actor, 1) == ORES_NATIVE_TASK_INVALID_ACTOR);
    assert(ores_native_vm_try_schedule_task(
        vm, (enum OresNativeVMDomain)-1, d[0].actor, 1)
        == ORES_NATIVE_TASK_INVALID_ACTOR);

    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        assert(ores_native_vm_try_send(vm, d[i].domain, d[i].actor,
            (uint64_t)i + 1u) == ORES_NATIVE_SEND_ACCEPTED);
    }

    /* MAIN: an already-running TASK is a physical DONE barrier after STOP. */
    until(&d[ORES_NATIVE_VM_MAIN].running, 1);
    assert(ores_native_vm_stop_sealed(vm, ORES_NATIVE_VM_MAIN, d[0].actor));
    assert(!ores_native_vm_poll_done(vm, ORES_NATIVE_VM_MAIN, d[0].actor));
    atomic_store(&d[0].release, 1);

    /* SHARED: queued TASK is canceled, never executed after STOP. */
    until(&d[ORES_NATIVE_VM_SHARED].data_entered, 1);
    assert(ores_native_vm_stop_sealed(vm, ORES_NATIVE_VM_SHARED, d[1].actor));
    assert(!ores_native_vm_poll_done(vm, ORES_NATIVE_VM_SHARED, d[1].actor));
    atomic_store(&d[1].release, 1);

    /* TRUSTED_PRIVATE: nested tasks remain on the owning actor's own pool. */
    until(&d[ORES_NATIVE_VM_TRUSTED_PRIVATE].task_runs, 2);
    assert(ores_native_vm_stop_sealed(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE, d[2].actor));
    assert(ores_native_vm_stop_sealed(
        vm, ORES_NATIVE_VM_UNTRUSTED, d[3].actor));

    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        assert(ores_native_vm_wait_done(vm, d[i].domain, d[i].actor));
        assert(atomic_load(&d[i].active) == 0);
    }
    assert(atomic_load(&d[0].task_runs) == 1);
    assert(atomic_load(&d[1].task_runs) == 0);
    assert(atomic_load(&d[1].task_cancels) == 1);
    assert(atomic_load(&d[2].task_runs) == 2);
    assert(atomic_load(&d[2].nested) == 1);
    assert(atomic_load(&d[3].task_runs) == 0);

    assert(ores_native_vm_close(vm));
    puts("NATIVE_VM_ACTOR_TASK_BRIDGE_PASS running=1 cancelled=1 nested=1");
    return 0;
}
