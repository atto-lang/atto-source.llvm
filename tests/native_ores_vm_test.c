#include "oreslang/runtime/NativeOresVM.h"
#include <stdatomic.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    atomic_int messages;
    atomic_int control;
    atomic_int parallel_messages;
    atomic_int attempted_close;
    atomic_int blocked_entered;
    atomic_int unblock;
    OresNativeVM* vm;
} State;
static void require(int ok) { if (!ok) abort(); }
static void deliver(void* opaque, size_t actor, uint64_t value) {
    State* state = (State*)opaque;
    if (actor == 1) {
        if (value != 43) abort();
        atomic_fetch_add(&state->parallel_messages, 1);
        return;
    }
    if (actor != 0) abort();
    if (value == 99) {
        if (!state->vm) abort();
        atomic_store(&state->attempted_close, ores_native_vm_close(state->vm));
        return;
    }
    if (value == 123) {
        atomic_store(&state->blocked_entered, 1);
        while (!atomic_load(&state->unblock)) sched_yield();
        return;
    }
    if (value != 42) abort();
    atomic_fetch_add(&state->messages, 1);
}
static void control(void* opaque, size_t actor, uint64_t value) {
    State* state = (State*)opaque;
    if (actor != 0 || value != 7) abort();
    atomic_fetch_add(&state->control, 1);
}
int main(void) {
    OresNativeVMConfig cfg = {0};
    State state[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        cfg.domains[i].actor_slots = 1;
        cfg.domains[i].mailbox_capacity = 4;
        cfg.domains[i].on_message = deliver;
        cfg.domains[i].on_control = control;
        cfg.domains[i].context = &state[i];
    }
    cfg.domains[ORES_NATIVE_VM_SHARED].carrier_workers = 2;
    cfg.domains[ORES_NATIVE_VM_MAIN].actor_slots = 2;
    OresNativeVM* vm = ores_native_vm_open(&cfg);
    require(vm != NULL);
    OresNativeActorRef refs[ORES_NATIVE_VM_DOMAIN_COUNT];
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain kind = (enum OresNativeVMDomain)i;
        require(ores_native_vm_workers(vm, kind) == (i == ORES_NATIVE_VM_SHARED ? 2u : 5u));
        require(ores_native_vm_started_carriers(vm, kind) == 0);
        refs[i] = ores_native_vm_spawn(vm, kind, 0);
        require(ores_native_vm_started_carriers(vm, kind) == 0);
        require(refs[i].scheduler_domain != 0);
        require(ores_native_vm_try_send(vm, kind, refs[i], 42) == ORES_NATIVE_SEND_ACCEPTED);
        require(ores_native_vm_publish_control(vm, kind, refs[i], 7));
    }
    /* Every distinct domain pair must reject the other's ActorRef, even
     * when the numeric actor slot and generation are identical. Check all
     * ingress paths, not only UNTRUSTED accepting a MAIN ref. */
    for (int destination = 0; destination < ORES_NATIVE_VM_DOMAIN_COUNT; ++destination) {
        for (int source = 0; source < ORES_NATIVE_VM_DOMAIN_COUNT; ++source) {
            if (source == destination) continue;
            enum OresNativeVMDomain kind = (enum OresNativeVMDomain)destination;
            require(ores_native_vm_try_send(vm, kind, refs[source], 42)
                == ORES_NATIVE_SEND_INVALID_REF);
            require(!ores_native_vm_publish_control(vm, kind, refs[source], 7));
            require(!ores_native_vm_stop(vm, kind, refs[source]));
        }
    }
    /* A forged enum must never select a different scheduler by array access. */
    for (int bad = -1; bad <= ORES_NATIVE_VM_DOMAIN_COUNT; bad +=
            ORES_NATIVE_VM_DOMAIN_COUNT + 1) {
        enum OresNativeVMDomain kind = (enum OresNativeVMDomain)bad;
        require(ores_native_vm_workers(vm, kind) == 0);
        require(ores_native_vm_spawn(vm, kind, 0).scheduler_domain == 0);
        require(ores_native_vm_try_send(vm, kind, refs[0], 42)
            == ORES_NATIVE_SEND_INVALID_REF);
        require(!ores_native_vm_publish_control(vm, kind, refs[0], 7));
        require(!ores_native_vm_stop(vm, kind, refs[0]));
    }
    OresNativeActorRef other_main = ores_native_vm_spawn(vm, ORES_NATIVE_VM_MAIN, 1);
    require(other_main.scheduler_domain == refs[ORES_NATIVE_VM_MAIN].scheduler_domain);
    require(other_main.actor_slot != refs[ORES_NATIVE_VM_MAIN].actor_slot);
    /* A busy single actor with a full mailbox still permits only one
     * independent handler turn. Do not provision additional idle carriers
     * merely because that actor accumulated queued DATA. */
    State* one = &state[ORES_NATIVE_VM_MAIN];
    for (int tries = 0; tries < 1000000
         && (atomic_load(&one->messages) != 1 ||
             atomic_load(&one->control) != 1); ++tries) sched_yield();
    require(atomic_load(&one->messages) == 1 && atomic_load(&one->control) == 1);
    require(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN,
        refs[ORES_NATIVE_VM_MAIN], 123) == ORES_NATIVE_SEND_ACCEPTED);
    for (int tries = 0; tries < 1000000
         && !atomic_load(&one->blocked_entered); ++tries) sched_yield();
    require(atomic_load(&one->blocked_entered));
    for (int i = 0; i < 3; ++i)
        require(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN,
            refs[ORES_NATIVE_VM_MAIN], 42) == ORES_NATIVE_SEND_ACCEPTED);
    require(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 1);
    /* A second independent actor lane must start a second carrier and run
     * even while the first actor's turn remains blocked. */
    require(ores_native_vm_try_send(vm, ORES_NATIVE_VM_MAIN, other_main, 43)
        == ORES_NATIVE_SEND_ACCEPTED);
    for (int tries = 0; tries < 1000000
         && !atomic_load(&one->parallel_messages); ++tries) sched_yield();
    require(atomic_load(&one->parallel_messages) == 1);
    require(ores_native_vm_started_carriers(vm, ORES_NATIVE_VM_MAIN) == 2);
    /* Keep MAIN actor 0 blocked while a different domain attempts to
     * close the VM. This specifically proves the global self-carrier
     * preflight runs BEFORE any cross-domain drain or pthread join. */
    require(!atomic_load(&one->unblock));

    /* An UNTRUSTED worker must never synchronously drain/join its own VM.
     * Even if MAIN workers are busy, callback-originated close must fail fast. */
    State* hostile = &state[ORES_NATIVE_VM_UNTRUSTED];
    hostile->vm = vm;
    atomic_store(&hostile->attempted_close, 2);
    require(ores_native_vm_try_send(
        vm, ORES_NATIVE_VM_UNTRUSTED,
        refs[ORES_NATIVE_VM_UNTRUSTED], 99) == ORES_NATIVE_SEND_ACCEPTED);
    /* Do not race a host stop against the worker-originated retirement. */
    for (int tries = 0; tries < 1000000
         && atomic_load(&hostile->attempted_close) == 2; ++tries) sched_yield();
    require(atomic_load(&hostile->attempted_close) == 0);
    /* Closure returned despite MAIN still having an active blocked turn.
     * The untrusted callback may retire but MUST NOT wait for that turn. */
    require(atomic_load(&one->blocked_entered));
    require(!atomic_load(&one->unblock));
    require(atomic_load(&one->messages) == 1);
    /* Callback close retires every domain but cannot join its own carrier.
     * Other domains must reject new work while accepted work drains.
     * No worker pool may exceed its configured carrier budget. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        enum OresNativeVMDomain kind = (enum OresNativeVMDomain)i;
        size_t started = ores_native_vm_started_carriers(vm, kind);
        require(started >= 1 && started <= ores_native_vm_workers(vm, kind));
        require(ores_native_vm_try_send(vm, kind, refs[i], 42)
            == ORES_NATIVE_SEND_TERMINATED);
        require(!ores_native_vm_publish_control(vm, kind, refs[i], 7));
        require(ores_native_vm_spawn(vm, kind, 0).scheduler_domain == 0);
    }
    /* Retire is idempotent; no new admissions after retirement. */
    ores_native_vm_retire(vm);
    /* A retired VM must still deliver already admitted DATA; release the
     * blocked carrier before external drain/close joins its pool. */
    atomic_store(&one->unblock, 1);
    /* External submitters are fenced: now a host thread can safely finish. */
    require(ores_native_vm_close(vm));
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        require(atomic_load(&state[i].messages) ==
            (i == ORES_NATIVE_VM_MAIN ? 4 : 1));
        require(atomic_load(&state[i].parallel_messages) ==
            (i == ORES_NATIVE_VM_MAIN ? 1 : 0));
        require(atomic_load(&state[i].control) == 1);
    }
    /* Failure while opening a later domain must reclaim every earlier
     * metadata-only scheduler without allocating any carrier threads. */
    OresNativeVMConfig invalid = cfg;
    invalid.domains[ORES_NATIVE_VM_UNTRUSTED].on_message = NULL;
    require(ores_native_vm_open(&invalid) == NULL);
    invalid = cfg;
    invalid.domains[ORES_NATIVE_VM_UNTRUSTED].carrier_workers = 129;
    require(ores_native_vm_open(&invalid) == NULL);
    puts("NATIVE_ORES_VM_PASS domains=4 default_workers=5 control=priority");
    return 0;
}
