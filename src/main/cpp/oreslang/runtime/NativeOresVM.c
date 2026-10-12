#include "oreslang/runtime/NativeOresVM.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct {
    OresNativeActorRef child;
    OresNativeActorRef supervisor;
    int terminal_published;
} OresVMSupervisorRegistration;

struct OresNativeVM {
    OresNativeActorScheduler* schedulers[ORES_NATIVE_VM_DOMAIN_COUNT];
    OresVMSupervisorRegistration* supervisors[ORES_NATIVE_VM_DOMAIN_COUNT];
    size_t actor_slots[ORES_NATIVE_VM_DOMAIN_COUNT];
    size_t workers[ORES_NATIVE_VM_DOMAIN_COUNT];
    /* Metadata-only central CONTROL wake coordinator. No master pthread.
     * This is not the per-actor supervisor mail lane itself. */
    pthread_mutex_t control_lock;
    unsigned pending_control_wakes;
    int control_retired;
};

static int valid_domain(enum OresNativeVMDomain domain) {
    return domain >= ORES_NATIVE_VM_MAIN && domain < ORES_NATIVE_VM_DOMAIN_COUNT;
}

OresNativeVM* ores_native_vm_open(const OresNativeVMConfig* config) {
    if (!config) return NULL;
    OresNativeVM* vm = (OresNativeVM*)calloc(1, sizeof(*vm));
    if (!vm) return NULL;
    if (pthread_mutex_init(&vm->control_lock, NULL) != 0) {
        free(vm);
        return NULL;
    }
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        const OresNativeVMDomainConfig* p = &config->domains[i];
        size_t workers = p->carrier_workers ? p->carrier_workers : 5;
        /* Validate all host callbacks and slot/capacity budgets before use.
         * NativeActorScheduler additionally enforces its allocation limits. */
        if (!p->on_message || !p->actor_slots || !p->mailbox_capacity
            || workers > 128) goto failed;
        vm->schedulers[i] = ores_native_actor_scheduler_open_typed_control(
            p->actor_slots, p->mailbox_capacity, workers,
            p->on_message, p->on_control, p->on_failure, p->context);
        if (!vm->schedulers[i]) goto failed;
        vm->supervisors[i] = (OresVMSupervisorRegistration*)calloc(
            p->actor_slots, sizeof(*vm->supervisors[i]));
        if (!vm->supervisors[i]) goto failed;
        vm->actor_slots[i] = p->actor_slots;
        vm->workers[i] = workers;
    }
    return vm;
failed:
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (vm->schedulers[i])
            ores_native_actor_scheduler_retire(vm->schedulers[i]);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (vm->schedulers[i]) {
            if (!ores_native_actor_scheduler_drain(vm->schedulers[i])) abort();
            if (!ores_native_actor_scheduler_try_destroy(vm->schedulers[i])) abort();
        }
    if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        free(vm->supervisors[i]);
    free(vm);
    return NULL;
}

size_t ores_native_vm_started_carriers(
    OresNativeVM* vm, enum OresNativeVMDomain domain) {
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_started_carriers(vm->schedulers[domain]) : 0;
}
size_t ores_native_vm_workers(const OresNativeVM* vm, enum OresNativeVMDomain domain) {
    return vm && valid_domain(domain) ? vm->workers[domain] : 0;
}

OresNativeActorRef ores_native_vm_spawn(
    OresNativeVM* vm, enum OresNativeVMDomain domain, size_t actor_slot) {
    OresNativeActorRef invalid = {0, 0, 0};
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_spawn(vm->schedulers[domain], actor_slot) : invalid;
}


/* An OresNativeActorRef has a scheduler-domain generation, never VM-global
 * authority. Compare all fields; numeric slots alone are not identities. */
static int same_actor(OresNativeActorRef a, OresNativeActorRef b) {
    return a.scheduler_domain != 0
        && a.scheduler_domain == b.scheduler_domain
        && a.actor_slot == b.actor_slot && a.generation == b.generation;
}
static void control_lock(OresNativeVM* vm);
static void control_unlock(OresNativeVM* vm);

OresNativeActorRef ores_native_vm_spawn_supervised(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    size_t actor_slot, OresNativeActorRef supervisor) {
    OresNativeActorRef invalid = {0, 0, 0};
    if (!vm || !valid_domain(domain)) return invalid;
    control_lock(vm);
    OresNativeActorRef child = invalid;
    if (!vm->control_retired && actor_slot < vm->actor_slots[domain]
        && supervisor.actor_slot != (uint64_t)actor_slot
        && ores_native_actor_scheduler_poll_ready(vm->schedulers[domain], supervisor)) {
        /* Serializes with VM stop/retire. The scheduler atomically rejects a
         * reused slot; only a successful spawn can install parentage. */
        child = ores_native_actor_scheduler_spawn(vm->schedulers[domain], actor_slot);
        if (child.scheduler_domain) {
            OresVMSupervisorRegistration* entry =
                &vm->supervisors[domain][actor_slot];
            entry->child = child;
            entry->supervisor = supervisor;
        }
    }
    control_unlock(vm);
    return child;
}

OresNativeActorRef ores_native_vm_supervisor_of(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef child) {
    OresNativeActorRef invalid = {0, 0, 0};
    if (!vm || !valid_domain(domain)) return invalid;
    control_lock(vm);
    OresNativeActorRef owner = invalid;
    if (child.actor_slot < vm->actor_slots[domain]) {
        const OresVMSupervisorRegistration* entry =
            &vm->supervisors[domain][child.actor_slot];
        if (same_actor(entry->child, child)) owner = entry->supervisor;
    }
    control_unlock(vm);
    return owner;
}

int ores_native_vm_publish_supervised_failure(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorTerminalFailure failure) {
    if (!vm || !valid_domain(domain)) return 0;
    control_lock(vm);
    int accepted = 0;
    if (!vm->control_retired
        && failure.child.actor_slot < vm->actor_slots[domain]) {
        OresVMSupervisorRegistration* entry =
            &vm->supervisors[domain][failure.child.actor_slot];
        /* Do not publish while the child callback is active or queued:
         * the supervisor must not see a terminal event ahead of teardown.
         * The runtime caller is responsible for non-scheduler resources. */
        if (same_actor(entry->child, failure.child)
            && entry->supervisor.scheduler_domain != 0
            && !entry->terminal_published
            && ores_native_actor_scheduler_poll_done(
                vm->schedulers[domain], failure.child)
            && ores_native_actor_scheduler_publish_failure(
                vm->schedulers[domain], entry->supervisor, failure)) {
            entry->terminal_published = 1;
            accepted = 1;
        }
    }
    control_unlock(vm);
    return accepted;
}

OresNativeSendStatus ores_native_vm_try_send(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef ref, uint64_t payload) {
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_try_send(vm->schedulers[domain], ref, payload)
        : ORES_NATIVE_SEND_INVALID_REF;
}

int ores_native_vm_stop(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    if (!vm || !valid_domain(domain)) return 0;
    /* Keep stop ordered against supervised child registration so a child
     * cannot inherit a parent concurrently passing into retirement. */
    control_lock(vm);
    int ok = !vm->control_retired
        && ores_native_actor_scheduler_stop(vm->schedulers[domain], ref);
    control_unlock(vm);
    return ok;
}

int ores_native_vm_poll_done(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_poll_done(vm->schedulers[domain], ref);
}
int ores_native_vm_wait_done(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_wait_done(vm->schedulers[domain], ref);
}

int ores_native_vm_publish_control(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef parent, uint64_t payload) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_publish_control(
            vm->schedulers[domain], parent, payload);
}

static void control_lock(OresNativeVM* vm) {
    if (pthread_mutex_lock(&vm->control_lock) != 0) abort();
}
static void control_unlock(OresNativeVM* vm) {
    if (pthread_mutex_unlock(&vm->control_lock) != 0) abort();
}
int ores_native_vm_control_notify(
    OresNativeVM* vm, enum OresNativeVMDomain domain) {
    if (!vm || !valid_domain(domain)) return 0;
    control_lock(vm);
    int ok = !vm->control_retired;
    if (ok) vm->pending_control_wakes |= 1u << (unsigned)domain;
    control_unlock(vm);
    return ok;
}
unsigned ores_native_vm_control_pending(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    unsigned pending = vm->pending_control_wakes;
    control_unlock(vm);
    return pending;
}
unsigned ores_native_vm_control_pump(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    if (vm->control_retired) {
        control_unlock(vm);
        return 0;
    }
    unsigned pending = vm->pending_control_wakes;
    vm->pending_control_wakes = 0;
    unsigned signalled = 0;
    /* Lock order is master CONTROL -> pool scheduler. The pool never calls
     * back into the master while holding its lock. No guest callback occurs. */
    for (unsigned i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if ((pending & (1u << i)) &&
            ores_native_actor_scheduler_signal_control(vm->schedulers[i]))
            signalled |= 1u << i;
    control_unlock(vm);
    return signalled;
}
void ores_native_vm_retire(OresNativeVM* vm) {
    if (!vm) return;
    control_lock(vm);
    vm->control_retired = 1;
    vm->pending_control_wakes = 0;
    /* Retire while holding the master control fence; pump cannot race pool
     * retirement or deliver a wake after shutdown begins. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        ores_native_actor_scheduler_retire(vm->schedulers[i]);
    control_unlock(vm);
}

int ores_native_vm_close(OresNativeVM* vm) {
    if (!vm) return 0;
    ores_native_vm_retire(vm);
    /* Reject synchronous teardown from *any* owned carrier before waiting on
     * any domain. Without this preflight, a callback in UNTRUSTED can block
     * forever draining MAIN while MAIN waits for its callback to finish. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (ores_native_actor_scheduler_is_current_carrier(vm->schedulers[i]))
            return 0;
    /* No callback can synchronously join or destroy its own scheduler. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (!ores_native_actor_scheduler_drain(vm->schedulers[i])) return 0;
    /* Caller-owned API/producer fence is required before memory reclamation. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (!ores_native_actor_scheduler_try_destroy(vm->schedulers[i])) abort();
    if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        free(vm->supervisors[i]);
    free(vm);
    return 1;
}
