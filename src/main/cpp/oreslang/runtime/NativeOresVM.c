#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeCpuBudget.h"
#include "oreslang/runtime/NativePhysicalTurnGate.h"
#include <pthread.h>
#include <errno.h>
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
    OresNativePhysicalTurnGate physical_gate; /* opt-in, actor callbacks only */
    /* Metadata-only central CONTROL wake coordinator. No master pthread.
     * This is not the per-actor supervisor mail lane itself. */
    pthread_mutex_t control_lock;
    unsigned pending_control_wakes;
    /* Opt-in host CONTROL service. Never executes an actor callback. */
    pthread_cond_t control_ready;
    pthread_t main_control_thread;
    int main_control_started;
    uint64_t control_wakes_dispatched;
    int control_retired;
    int guest_actor_admitted;
    int realtime_bootstrap_attempted;
    int nio_bootstrap_in_progress;
    pthread_t nio_bootstrap_owner;
    void* nio_backend;
    OresNativeVMNioDestroy nio_destroy;
    size_t nio_borrows;
    pthread_cond_t nio_idle;
};

static int valid_domain(enum OresNativeVMDomain domain) {
    return domain >= ORES_NATIVE_VM_MAIN && domain < ORES_NATIVE_VM_DOMAIN_COUNT;
}

static OresNativeVM* open_vm_internal(
        const OresNativeVMConfig* config,
        const OresNativeVMWakeConfig* wakes) {
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
    if (pthread_cond_init(&vm->control_ready, NULL) != 0) {
        if (pthread_cond_destroy(&vm->nio_idle) != 0) abort();
        if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
        free(vm);
        return NULL;
    }
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        const OresNativeVMDomainConfig* p = &config->domains[i];
        const OresNativeActorWakeFn on_wake =
            wakes ? wakes->domains[i] : NULL;
        size_t workers = p->carrier_workers ? p->carrier_workers : 5;
        /* Validate all host callbacks and slot/capacity budgets before use.
         * NativeActorScheduler additionally enforces its allocation limits. */
        if (!p->on_message || !p->actor_slots || !p->mailbox_capacity
            || workers > 128) goto failed;
        vm->schedulers[i] = ores_native_actor_scheduler_open_with_wakes(
            p->actor_slots, p->mailbox_capacity, workers,
            p->on_message, p->on_control, p->on_failure, on_wake, p->context);
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
    if (pthread_cond_destroy(&vm->control_ready) != 0) abort();
    if (pthread_cond_destroy(&vm->nio_idle) != 0) abort();
    if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        free(vm->supervisors[i]);
    free(vm);
    return NULL;
}

OresNativeVM* ores_native_vm_open(const OresNativeVMConfig* config) {
    return open_vm_internal(config, NULL);
}

OresNativeVM* ores_native_vm_open_with_wakes(
        const OresNativeVMConfig* config,
        const OresNativeVMWakeConfig* wakes) {
    return config && wakes ? open_vm_internal(config, wakes) : NULL;
}

static void control_lock(OresNativeVM* vm);
static void control_unlock(OresNativeVM* vm);

int ores_native_vm_prepare_realtime(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    if (vm->control_retired || vm->guest_actor_admitted
            || vm->nio_bootstrap_in_progress || vm->realtime_bootstrap_attempted) {
        control_unlock(vm);
        return 0;
    }
    // One-way cold-host transition, including failure. Serialize with
    // spawn/retire so carrier creation cannot race guest admission or close.
    vm->realtime_bootstrap_attempted = 1;
    int ok = 1;
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        if (!ores_native_actor_scheduler_prestart_realtime(vm->schedulers[i])) {
            ok = 0;
            break;
        }
    }
    control_unlock(vm);
    return ok;
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

size_t ores_native_vm_effective_cpu_budget(void) {
    return ores_native_cpu_effective();
}

static void wake_physical_turn_domains(void* context) {
    OresNativeVM* vm = (OresNativeVM*)context;
    /* Called only after an actor turn has fully returned and its scheduler
     * mutex is released. This is not guest/supervisor CONTROL delivery. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        ores_native_actor_scheduler_signal_turn_gate(vm->schedulers[i]);
}

OresNativeVM* ores_native_vm_open_cpu_budgeted(const OresNativeVMConfig* config) {
    if (!config) return NULL;
    OresNativeVMConfig copy = *config;
    size_t requested[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};
    size_t allocated[ORES_NATIVE_VM_DOMAIN_COUNT] = {0};
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        requested[i] = copy.domains[i].carrier_workers;
    size_t physical_maximum = ores_native_cpu_effective();
    ores_native_cpu_distribute(physical_maximum, requested, allocated);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        copy.domains[i].carrier_workers = allocated[i];
    OresNativeVM* vm = ores_native_vm_open(&copy);
    if (!vm) return NULL;
    vm->physical_gate.maximum = physical_maximum;
    vm->physical_gate.context = vm;
    vm->physical_gate.notify_free = wake_physical_turn_domains;
    atomic_init(&vm->physical_gate.admitted, 0);
    atomic_init(&vm->physical_gate.waiting, 0);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        if (!ores_native_actor_scheduler_configure_turn_gate(
                vm->schedulers[i], &vm->physical_gate)) {
            if (!ores_native_vm_close(vm)) abort();
            return NULL;
        }
    }
    return vm;
}

OresNativeVM* ores_native_vm_open_with_physical_turn_limit(
        const OresNativeVMConfig* config, size_t maximum) {
    if (!config || maximum == 0) return NULL;
    OresNativeVM* vm = ores_native_vm_open(config);
    if (!vm) return NULL;
    vm->physical_gate.maximum = maximum;
    vm->physical_gate.context = vm;
    vm->physical_gate.notify_free = wake_physical_turn_domains;
    atomic_init(&vm->physical_gate.admitted, 0);
    atomic_init(&vm->physical_gate.waiting, 0);
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        if (!ores_native_actor_scheduler_configure_turn_gate(
                vm->schedulers[i], &vm->physical_gate)) {
            if (!ores_native_vm_close(vm)) abort();
            return NULL;
        }
    }
    return vm;
}

size_t ores_native_vm_active_physical_turns(const OresNativeVM* vm) {
    return vm && vm->physical_gate.maximum
        ? ores_native_turn_gate_active(&vm->physical_gate) : 0;
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
    control_lock(vm);
    OresNativeActorRef admitted = invalid;
    if (!vm->control_retired && !vm->nio_bootstrap_in_progress) {
        admitted = ores_native_actor_scheduler_spawn(vm->schedulers[domain], actor_slot);
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
        /* Per-child terminal receipts require scheduler-side parentage bound
         * before any USER admission; a fresh child cannot legitimately fail
         * this, so a refusal is a broken invariant, never a silent fallback. */
        if (child.scheduler_domain
            && !ores_native_actor_scheduler_bind_supervisor(
                vm->schedulers[domain], supervisor, child))
            abort();
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

static int vm_stop(
        OresNativeVM* vm,
        enum OresNativeVMDomain domain,
        OresNativeActorRef ref,
        int sealed) {
    if (!vm || !valid_domain(domain)) return 0;
    /* Keep stop ordered against supervised child registration so a child
     * cannot inherit a parent concurrently passing into retirement. */
    control_lock(vm);
    int ok = !vm->control_retired;
    /* A supervisor may stop only after every direct child has physically
     * completed and its terminal notice was admitted to the supervisor lane.
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
    if (ok) {
        ok = sealed
            ? ores_native_actor_scheduler_stop_sealed(
                vm->schedulers[domain], ref)
            : ores_native_actor_scheduler_stop(
                vm->schedulers[domain], ref);
    }
    control_unlock(vm);
    return ok;
}

int ores_native_vm_stop(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm_stop(vm, domain, ref, 0);
}

int ores_native_vm_stop_sealed(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm_stop(vm, domain, ref, 1);
}

int ores_native_vm_actor_resource_retain(
        OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_retain_resource(
            vm->schedulers[domain], ref);
}

int ores_native_vm_actor_resource_release(
        OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_release_resource(
            vm->schedulers[domain], ref);
}

int ores_native_vm_actor_reserve_wake(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeActorRef producer, OresNativeActorRef target,
        uint64_t continuation_slot, uint64_t continuation_generation,
        uint64_t code_generation, uint64_t descriptor_generation,
        OresNativeWakeTicket* out) {
    if (out) *out = (OresNativeWakeTicket){0};
    /* This VM surface deliberately does not expose the scheduler's internal
     * cross-scheduler target mode. A separate authenticated bridge is required
     * before a continuation may target another scheduler/domain. */
    return vm && valid_domain(domain) && out
        && producer.scheduler_domain != 0
        && target.scheduler_domain == producer.scheduler_domain
        && ores_native_actor_scheduler_reserve_wake(
            vm->schedulers[domain], producer, target,
            continuation_slot, continuation_generation,
            code_generation, descriptor_generation, out);
}
int ores_native_vm_actor_transfer_wake(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeWakeTicket ticket) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_transfer_wake(
            vm->schedulers[domain], ticket);
}
int ores_native_vm_actor_cancel_wake(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeWakeTicket ticket) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_cancel_wake(
            vm->schedulers[domain], ticket);
}
int ores_native_vm_actor_abort_transferred_wake(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeWakeTicket ticket) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_abort_transferred_wake(
            vm->schedulers[domain], ticket);
}
OresNativeWakePublishStatus ores_native_vm_actor_publish_wake(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeWakeTicket ticket, OresNativeWakeOutcome outcome) {
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_publish_wake(
            vm->schedulers[domain], ticket, outcome)
        : ORES_NATIVE_WAKE_INVALID;
}
int ores_native_vm_actor_is_current_wake(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeWakeTicket ticket) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_is_current_wake(
            vm->schedulers[domain], ticket);
}
size_t ores_native_vm_actor_outstanding_wake_loans(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeActorRef producer) {
    return vm && valid_domain(domain)
        ? ores_native_actor_scheduler_outstanding_wake_loans(
            vm->schedulers[domain], producer)
        : 0;
}

int ores_native_vm_poll_done(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef ref) {
    return vm && valid_domain(domain)
        && ores_native_actor_scheduler_poll_done(vm->schedulers[domain], ref);
}
int ores_native_vm_actor_terminal_state(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef ref, OresNativeActorTerminalState* output) {
    return vm && valid_domain(domain) && output
        && ores_native_actor_scheduler_terminal_state(
            vm->schedulers[domain], ref, output);
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

OresNativeActorFuture* ores_native_vm_actor_future_open(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeActorRef owner, uint64_t control_token) {
    return vm && valid_domain(domain)
        ? ores_native_actor_future_open(
            vm->schedulers[domain], owner, control_token)
        : NULL;
}

int ores_native_vm_current_execution_context(
        OresNativeVM* vm, enum OresNativeVMDomain domain,
        OresNativeExecutionContext* out) {
    return vm && valid_domain(domain) && out
        && ores_native_actor_scheduler_current_execution_context(
            vm->schedulers[domain], out);
}

static void control_lock(OresNativeVM* vm) {
    if (pthread_mutex_lock(&vm->control_lock) != 0) abort();
}
static void control_unlock(OresNativeVM* vm) {
    if (pthread_mutex_unlock(&vm->control_lock) != 0) abort();
}

/* One serialized CONTROL authority for manual host pump and native main loop.
 * No guest or supervisor callback runs on this thread. */
static unsigned dispatch_control_locked(OresNativeVM* vm) {
    const unsigned pending = vm->pending_control_wakes;
    vm->pending_control_wakes = 0;
    unsigned signalled = 0;
    for (unsigned i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i) {
        if ((pending & (1u << i))
                && ores_native_actor_scheduler_signal_control(vm->schedulers[i])) {
            signalled |= 1u << i;
            if (vm->control_wakes_dispatched != UINT64_MAX)
                ++vm->control_wakes_dispatched;
        }
    }
    return signalled;
}

static void* run_main_control_loop(void* opaque) {
    OresNativeVM* vm = (OresNativeVM*)opaque;
    control_lock(vm);
    while (!vm->control_retired) {
        while (!vm->control_retired && !vm->pending_control_wakes)
            if (pthread_cond_wait(&vm->control_ready, &vm->control_lock) != 0)
                abort();
        if (!vm->control_retired) (void)dispatch_control_locked(vm);
    }
    control_unlock(vm);
    return NULL;
}

int ores_native_vm_start_main_control_loop(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    /* The startup barrier is host-owned: never create CONTROL threads from a
     * guest actor turn, or race an in-progress NIO bootstrap or retirement. */
    /* A real-time bootstrap is a one-way thread provisioning barrier.
     * Starting this pthread after prepare_realtime() would violate the
     * no-late-thread contract, including after a failed prestart attempt.
     * A host that needs both services must start CONTROL before RT prestart. */
    if (vm->control_retired || vm->guest_actor_admitted
            || vm->nio_bootstrap_in_progress || vm->realtime_bootstrap_attempted) {
        control_unlock(vm);
        return 0;
    }
    if (vm->main_control_started) {
        control_unlock(vm);
        return 1;
    }
    const int created = pthread_create(
        &vm->main_control_thread, NULL, run_main_control_loop, vm) == 0;
    if (created) vm->main_control_started = 1;
    control_unlock(vm);
    return created;
}

int ores_native_vm_main_control_loop_started(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    const int result = vm->main_control_started;
    control_unlock(vm);
    return result;
}

uint64_t ores_native_vm_control_wakes_dispatched(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    const uint64_t result = vm->control_wakes_dispatched;
    control_unlock(vm);
    return result;
}

int ores_native_vm_control_notify(
    OresNativeVM* vm, enum OresNativeVMDomain domain) {
    if (!vm || !valid_domain(domain)) return 0;
    control_lock(vm);
    int ok = !vm->control_retired;
    if (ok) {
        vm->pending_control_wakes |= 1u << (unsigned)domain;
        if (vm->main_control_started
                && pthread_cond_signal(&vm->control_ready) != 0) abort();
    }
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
    /* Manual pump remains available for legacy host event loops. The native
     * CONTROL thread may have already consumed the coalesced notification. */
    const unsigned signalled = dispatch_control_locked(vm);
    control_unlock(vm);
    return signalled;
}
int ores_native_vm_nio_can_attach(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    int ok = !vm->control_retired && !vm->guest_actor_admitted
        && !vm->nio_bootstrap_in_progress && !vm->nio_backend;
    control_unlock(vm);
    return ok;
}

int ores_native_vm_nio_begin_bootstrap(OresNativeVM* vm) {
    if (!vm) return 0;
    control_lock(vm);
    int granted = !vm->control_retired && !vm->guest_actor_admitted
        && !vm->nio_backend && !vm->nio_bootstrap_in_progress;
    if (granted) {
        vm->nio_bootstrap_in_progress = 1;
        vm->nio_bootstrap_owner = pthread_self();
    }
    control_unlock(vm);
    return granted;
}

void ores_native_vm_nio_cancel_bootstrap(OresNativeVM* vm) {
    if (!vm) return;
    control_lock(vm);
    if (vm->nio_bootstrap_in_progress
            && pthread_equal(vm->nio_bootstrap_owner, pthread_self())) {
        vm->nio_bootstrap_in_progress = 0;
        if (vm->control_retired
                && pthread_cond_broadcast(&vm->nio_idle) != 0) abort();
    }
    control_unlock(vm);
}

int ores_native_vm_attach_nio(
    OresNativeVM* vm, void* backend, OresNativeVMNioDestroy destroy) {
    if (!vm || !backend || !destroy) return 0;
    control_lock(vm);
    const int owner = vm->nio_bootstrap_in_progress
        && pthread_equal(vm->nio_bootstrap_owner, pthread_self());
    const int accepted = owner && !vm->control_retired
        && !vm->guest_actor_admitted && !vm->nio_backend;
    if (accepted) {
        vm->nio_backend = backend;
        vm->nio_destroy = destroy;
    }
    if (owner) {
        vm->nio_bootstrap_in_progress = 0;
        if (vm->control_retired
                && pthread_cond_broadcast(&vm->nio_idle) != 0) abort();
    }
    control_unlock(vm);
    return accepted;
}

void* ores_native_vm_nio_acquire(OresNativeVM* vm) {
    if (!vm) return NULL;
    int status = pthread_mutex_trylock(&vm->control_lock);
    if (status == EBUSY) return NULL;
    if (status != 0) abort();
    void* backend = NULL;
    if (!vm->control_retired && vm->nio_backend
            && vm->nio_borrows != (size_t)-1) {
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
    if (vm->main_control_started
            && pthread_cond_broadcast(&vm->control_ready) != 0) abort();
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
    /* Retirement woke the blocking CONTROL loop; join it before destroying
     * the pool schedulers it can signal. Only a trusted external host closes. */
    if (vm->main_control_started) {
        if (pthread_equal(pthread_self(), vm->main_control_thread)) return 0;
        if (pthread_join(vm->main_control_thread, NULL) != 0) abort();
        vm->main_control_started = 0;
    }
    /* No callback can synchronously join or destroy its own scheduler. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (!ores_native_actor_scheduler_drain(vm->schedulers[i])) return 0;

    /* External-host close owns the final NIO join. Retirement has already
     * fenced new acquisitions; wait for previously accepted operations and
     * any owner-bound cold-start reservation before detaching the backend. */
    control_lock(vm);
    while (vm->nio_borrows || vm->nio_bootstrap_in_progress) {
        if (pthread_cond_wait(&vm->nio_idle, &vm->control_lock) != 0) abort();
    }
    void* nio_backend = vm->nio_backend;
    OresNativeVMNioDestroy nio_destroy = vm->nio_destroy;
    vm->nio_backend = NULL;
    vm->nio_destroy = NULL;
    control_unlock(vm);
    if (nio_backend) nio_destroy(nio_backend);

    /* No callback can synchronously join or destroy its own scheduler. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (!ores_native_actor_scheduler_drain(vm->schedulers[i])) return 0;

    /* Caller-owned API/producer fence is required before memory reclamation. */
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        if (!ores_native_actor_scheduler_try_destroy(vm->schedulers[i])) abort();
    if (pthread_cond_destroy(&vm->control_ready) != 0) abort();
    if (pthread_cond_destroy(&vm->nio_idle) != 0) abort();
    if (pthread_mutex_destroy(&vm->control_lock) != 0) abort();
    for (int i = 0; i < ORES_NATIVE_VM_DOMAIN_COUNT; ++i)
        free(vm->supervisors[i]);
    free(vm);
    return 1;
}
