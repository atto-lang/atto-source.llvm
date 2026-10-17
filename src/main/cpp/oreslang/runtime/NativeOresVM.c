#include "oreslang/runtime/NativeOresVM.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct {
    OresNativeActorRef child;
    OresNativeActorRef supervisor;
    int terminal_published;
    int host_finalized;
} OresVMSupervisorRegistration;

struct OresNativeVM {
    OresNativeActorScheduler* schedulers[ORES_NATIVE_VM_DOMAIN_COUNT];
    OresVMSupervisorRegistration* supervisors[ORES_NATIVE_VM_DOMAIN_COUNT];
    size_t actor_slots[ORES_NATIVE_VM_DOMAIN_COUNT];
    int typed_failure_enabled[ORES_NATIVE_VM_DOMAIN_COUNT];
    size_t workers[ORES_NATIVE_VM_DOMAIN_COUNT];
    /* Metadata-only central CONTROL wake coordinator. No master pthread.
     * This is not the per-actor supervisor mail lane itself. */
    pthread_mutex_t control_lock;
    unsigned pending_control_wakes;
    int control_retired;
    int guest_actor_admitted; /* NIO bootstrap barrier: no late reactor creation */
    int nio_bootstrap_in_progress; /* CONTROL-gated cold-start reservation */
    void* nio_backend;
    OresNativeVMNioDestroy nio_destroy;
    /* Under control_lock. A backend borrow pins accepted host operations
     * until external-host VM close has fenced and drained them. */
    size_t nio_borrows;
    pthread_cond_t nio_idle;
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
    if (pthread_cond_init(&vm->nio_idle, NULL) != 0) {
        if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
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
        vm->typed_failure_enabled[i] = p->on_failure != NULL;
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
    if (pthread_cond_destroy(&vm->nio_idle) != 0) abort();
    if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        free(vm->supervisors[i]);
    free(vm);
    return NULL;
}

/* Caller must not publish the VM to producers before this completes.
 * All four domains must be cold: if any prestart fails, the caller must
 * retire the partially prestarted VM instead of entering service.
 */
int ores_native_vm_prepare_realtime(OresNativeVM* vm) {
    if (!vm) return 0;
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        if (!ores_native_actor_scheduler_prestart_realtime(vm->schedulers[i]))
            return 0;
    }
    return 1;
}

OresNativeVM* ores_native_vm_open_realtime(const OresNativeVMConfig* config) {
    OresNativeVM* vm = ores_native_vm_open(config);
    if (!vm) return NULL;
    if (!ores_native_vm_prepare_realtime(vm)) {
        if (!ores_native_vm_close(vm)) abort();
        return NULL;
    }
    return vm;
}

size_t ores_native_vm_started_carriers(
    OresNativeVM* vm, enum OresNativeVMDomain domain) {
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_started_carriers(vm->schedulers[domain]) : 0;
}
size_t ores_native_vm_workers(const OresNativeVM* vm, enum OresNativeVMDomain domain) {
    return vm && valid_domain(domain) ? vm->workers[domain] : 0;
}

int ores_native_vm_has_hard_security_boundary(
    const OresNativeVM* vm, enum OresNativeVMDomain domain) {
    /* Four queues and four carrier pools cannot isolate memory or native
     * pointers. Do not return true merely because the domain is UNTRUSTED. */
    (void)vm;
    (void)domain;
    return 0;
}

static void control_lock(OresNativeVM* vm);
static void control_unlock(OresNativeVM* vm);

OresNativeActorRef ores_native_vm_spawn(
    OresNativeVM* vm, enum OresNativeVMDomain domain, size_t actor_slot) {
    OresNativeActorRef invalid = {0, 0, 0};
    if (!vm || !valid_domain(domain)) return invalid;
    // Shared lock order CONTROL -> actor scheduler. Bootstrap attachment must
    // never race first guest spawn and silently add reactor threads later.
    control_lock(vm);
    OresNativeActorRef admitted = invalid;
    if (!vm->control_retired && !vm->nio_bootstrap_in_progress) {
        admitted = ores_native_actor_scheduler_spawn(
            vm->schedulers[domain], actor_slot);
        if (admitted.scheduler_domain) vm->guest_actor_admitted = 1;
    }
    control_unlock(vm);
    return admitted;
}


/* An OresNativeActorRef has a scheduler-domain generation, never VM-global
 * authority. Compare all fields; numeric slots alone are not identities. */
static int same_actor(OresNativeActorRef a, OresNativeActorRef b) {
    return a.scheduler_domain != 0
        && a.scheduler_domain == b.scheduler_domain
        && a.actor_slot == b.actor_slot && a.generation == b.generation;
}

OresNativeActorRef ores_native_vm_spawn_supervised(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    size_t actor_slot, OresNativeActorRef supervisor) {
    OresNativeActorRef invalid = {0, 0, 0};
    if (!vm || !valid_domain(domain)) return invalid;
    control_lock(vm);
    OresNativeActorRef child = invalid;
    if (!vm->control_retired && !vm->nio_bootstrap_in_progress
        && vm->typed_failure_enabled[domain]
        && actor_slot < vm->actor_slots[domain]
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
            vm->guest_actor_admitted = 1;
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

/* Explicit trusted-host completion receipt after external finalizers run.
 * Scheduler DONE is necessary but cannot prove host-owned cleanup itself. */
int ores_native_vm_confirm_supervised_finalized(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef child) {
    if (!vm || !valid_domain(domain)) return 0;
    control_lock(vm);
    int accepted = 0;
    if (!vm->control_retired && child.actor_slot < vm->actor_slots[domain]) {
        OresVMSupervisorRegistration* entry =
            &vm->supervisors[domain][child.actor_slot];
        if (same_actor(entry->child, child) && !entry->host_finalized
            && !entry->terminal_published
            && ores_native_actor_scheduler_poll_done(
                vm->schedulers[domain], child)) {
            entry->host_finalized = 1;
            accepted = 1;
        }
    }
    control_unlock(vm);
    return accepted;
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
            && entry->host_finalized
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
    int ok = !vm->control_retired;
    /* A supervisor may stop only after every direct child has physically
     * drained AND its terminal notice was admitted to the supervisor lane.
     * Reject early parent retirement rather than silently orphaning a live
     * child. Every operation here acquires scheduler locks under the same
     * master -> scheduler ordering as spawn and terminal admission. */
    if (ok) {
        for (size_t i = 0; i < vm->actor_slots[domain]; ++i) {
            const OresVMSupervisorRegistration* child = &vm->supervisors[domain][i];
            if (same_actor(child->supervisor, ref)
                && (!child->terminal_published
                    || !ores_native_actor_scheduler_poll_done(
                        vm->schedulers[domain], child->child))) {
                ok = 0;
                break;
            }
        }
    }
    if (ok)
        ok = ores_native_actor_scheduler_stop(vm->schedulers[domain], ref);
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
/* Trusted bootstrap probe. A concurrent first actor spawn can still
 * win before the later final attachment CAS; the host must fence producers
 * during initialization. Check again inside attach_nio under CONTROL lock.
 */
int ores_native_vm_nio_can_attach(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    int ok = !vm->control_retired && !vm->guest_actor_admitted
        && !vm->nio_bootstrap_in_progress && !vm->nio_backend;
    control_unlock(vm);
    return ok;
}

/* Reserve the cold-start NIO phase before creating OS descriptors or
 * selector threads. Guest actor spawn fails admission while reserved; it
 * cannot race an in-progress selector bootstrap into a live actor profile.
 * These trusted host functions do not allocate memory or wait on reactors.
 */
int ores_native_vm_nio_begin_bootstrap(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    int granted = !vm->control_retired && !vm->guest_actor_admitted
        && !vm->nio_backend && !vm->nio_bootstrap_in_progress;
    if (granted) vm->nio_bootstrap_in_progress = 1;
    control_unlock(vm);
    return granted;
}
void ores_native_vm_nio_cancel_bootstrap(OresNativeVM* vm) {
    if (!vm) return;
    control_lock(vm);
    vm->nio_bootstrap_in_progress = 0;
    control_unlock(vm);
}

/* The VM owns the opaque selector backend, but never calls a C++ symbol
 * from its C scheduler core. This preserves standalone C-only host tests.
 * Attachment is linearized against retirement under the VM CONTROL lock.
 */
int ores_native_vm_attach_nio(
    OresNativeVM* vm, void* backend, OresNativeVMNioDestroy destroy) {
    if (!vm || !backend || !destroy) return 0;
    control_lock(vm);
    const int accepted = vm->nio_bootstrap_in_progress
        && !vm->control_retired && !vm->guest_actor_admitted && !vm->nio_backend;
    if (accepted) {
        vm->nio_backend = backend;
        vm->nio_destroy = destroy;
    }
    vm->nio_bootstrap_in_progress = 0;
    control_unlock(vm);
    return accepted;
}
/* A borrowed pointer is valid until its exactly-once release. This is a
 * trusted-host operation lease, never transferable to guest actor code.
 * Final VM close still requires an external producer/API-entry fence.
 */
void* ores_native_vm_nio_acquire(OresNativeVM* vm) {
    if (!vm) return NULL;
    control_lock(vm);
    void* backend = NULL;
    if (!vm->control_retired && vm->nio_backend && vm->nio_borrows != (size_t)-1) {
        ++vm->nio_borrows;
        backend = vm->nio_backend;
    }
    control_unlock(vm);
    return backend;
}
void ores_native_vm_nio_release(OresNativeVM* vm, void* backend) {
    if (!vm || !backend) abort();
    control_lock(vm);
    if (!vm->nio_borrows || backend != vm->nio_backend) abort();
    if (--vm->nio_borrows == 0 && vm->control_retired) {
        if (pthread_cond_broadcast(&vm->nio_idle) != 0) abort();
    }
    control_unlock(vm);
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
    // Only the externally fenced host closer waits here; actor, CONTROL and
    // epoll carriers never park while attempting guest I/O admission.
    control_lock(vm);
    while (vm->nio_borrows) {
        if (pthread_cond_wait(&vm->nio_idle, &vm->control_lock) != 0) abort();
    }
    void* backend = vm->nio_backend;
    OresNativeVMNioDestroy destroy = vm->nio_destroy;
    vm->nio_backend = NULL;
    vm->nio_destroy = NULL;
    control_unlock(vm);
    if (backend) destroy(backend);
    if (pthread_cond_destroy(&vm->nio_idle) != 0) abort();
    if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        free(vm->supervisors[i]);
    free(vm);
    return 1;
}
