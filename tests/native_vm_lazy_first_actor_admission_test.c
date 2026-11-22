#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeOresVM.h"
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* Real NativeOresVM + NativeActorScheduler/POSIX pthread integration.
 * This is a host-side parity test; it does not claim an independent VM
 * sandbox or successful compilation of a source-language actor program. */
typedef struct {
    atomic_int blocked_entered;
    atomic_int release_blocked;
    atomic_int parallel_executed;
} TestState;

static void check(int condition) {
    if (!condition) abort();
}
static void until(atomic_int* value) {
    struct timespec now;
    check(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    time_t deadline = now.tv_sec + 5;
    while (!atomic_load_explicit(value, memory_order_acquire)) {
        check(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        check(now.tv_sec <= deadline);
        sched_yield();
    }
}
static void on_failure(void* context, size_t parent,
                       OresNativeActorTerminalFailure failure) {
    (void)context;
    (void)parent;
    (void)failure;
    /* Typed failure delivery is verified by supervisor lifecycle tests;
     * this fixture exercises only spawn-side parent registration. */
}
static void on_message(void* context, size_t actor, uint64_t msg) {
    TestState* state = (TestState*)context;
    if (actor == 0 && msg == 100) {
        atomic_store_explicit(&state->blocked_entered, 1, memory_order_release);
        while (!atomic_load_explicit(&state->release_blocked, memory_order_acquire))
            sched_yield();
    } else if (actor == 1 && msg == 200) {
        atomic_fetch_add_explicit(&state->parallel_executed, 1, memory_order_release);
    } else {
        abort();
    }
}

int main(void) {
    OresNativeVMConfig cfg = {0};
    TestState states[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = 64;
        cfg.domains[i].mailbox_capacity = 8;
        cfg.domains[i].carrier_workers = 3;
        cfg.domains[i].on_message = on_message;
        cfg.domains[i].on_failure = on_failure;
        cfg.domains[i].context = &states[i];
    }

    OresNativeVM* vm = ores_native_vm_open(&cfg);
    check(vm != NULL);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        check(ores_native_vm_started_carriers(vm, (enum OresNativeVMDomain)i) == 0);

    /* Fault-injected first pthread creation must not consume the actor slot
     * or publish an unusable ActorRef. Retry must start exactly one worker. */
    ores_native_actor_scheduler_test_fail_next_create();
    check(!ores_native_vm_spawn_ready(vm, ORES_NATIVE_VM_SHARED, 0).scheduler_domain);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_SHARED) == 0);
    OresNativeActorRef first =
        ores_native_vm_spawn_ready(vm, ORES_NATIVE_VM_SHARED, 0);
    check(first.scheduler_domain && first.actor_slot == 0);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_SHARED) == 1);

    /* 63 additional idle actor identities must reuse that one native worker.
     * They cannot start one OS thread per actor; no guest callbacks run. */
    OresNativeActorRef second = {0};
    for (size_t slot = 1; slot < 64; ++slot) {
        OresNativeActorRef actor =
            ores_native_vm_spawn_ready(vm, ORES_NATIVE_VM_SHARED, slot);
        check(actor.scheduler_domain && actor.actor_slot == slot);
        if (slot == 1) second = actor;
    }
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_SHARED) == 1);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 0);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_TRUSTED_PRIVATE) == 0);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_UNTRUSTED) == 0);

    /* Other domains have distinct carrier accounting. This is a scheduling
     * bulkhead, not a native security isolation boundary. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain kind = (enum OresNativeVMDomain)i;
        check(!ores_native_vm_has_hard_security_boundary(vm, kind));
        if (kind == ORES_NATIVE_VM_SHARED) continue;
        OresNativeActorRef actor = ores_native_vm_spawn_ready(vm, kind, 0);
        check(actor.scheduler_domain);
        check(ores_native_vm_started_carriers(vm, kind) == 1);
    }

    /* A busy shared actor occupies its own carrier. Failed growth must not
     * accept a second independent lane. Retry grows only to two, bounded by
     * the configured three workers, and runs while actor 0 is still blocked. */
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_SHARED, first, 100)
          == ORES_NATIVE_SEND_ACCEPTED);
    until(&states[ORES_NATIVE_VM_SHARED].blocked_entered);
    ores_native_actor_scheduler_test_fail_next_create();
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_SHARED, second, 200)
          == ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_SHARED) == 1);
    check(atomic_load(&states[ORES_NATIVE_VM_SHARED].parallel_executed) == 0);
    check(ores_native_vm_try_send(vm, ORES_NATIVE_VM_SHARED, second, 200)
          == ORES_NATIVE_SEND_ACCEPTED);
    until(&states[ORES_NATIVE_VM_SHARED].parallel_executed);
    check(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_SHARED) == 2);
    check(!atomic_load(&states[ORES_NATIVE_VM_SHARED].release_blocked));

    atomic_store_explicit(&states[ORES_NATIVE_VM_SHARED].release_blocked, 1,
                          memory_order_release);
    check(ores_native_vm_close(vm));

    /* A metadata-only supervisor does not prestart a carrier. An injected
     * failure on ready-child publication must not create an orphan child or
     * supervisor-of registration. A retry for the same slot succeeds. */
    vm = ores_native_vm_open(&cfg);
    check(vm != NULL);
    OresNativeActorRef supervisor = ores_native_vm_spawn(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE, 0);
    check(supervisor.scheduler_domain);
    check(ores_native_vm_started_carriers(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE) == 0);
    ores_native_actor_scheduler_test_fail_next_create();
    OresNativeActorRef failed = ores_native_vm_spawn_supervised_ready(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE, 1, supervisor);
    check(!failed.scheduler_domain);
    check(ores_native_vm_started_carriers(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE) == 0);
    OresNativeActorRef child = ores_native_vm_spawn_supervised_ready(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE, 1, supervisor);
    check(child.scheduler_domain && child.actor_slot == 1);
    check(ores_native_vm_started_carriers(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE) == 1);
    OresNativeActorRef parent = ores_native_vm_supervisor_of(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE, child);
    check(parent.scheduler_domain == supervisor.scheduler_domain);
    check(parent.actor_slot == supervisor.actor_slot);
    check(parent.generation == supervisor.generation);
    check(!ores_native_vm_spawn_supervised_ready(
        vm, ORES_NATIVE_VM_TRUSTED_PRIVATE, 1, supervisor).scheduler_domain);
    check(ores_native_vm_close(vm));
    return 0;
}
