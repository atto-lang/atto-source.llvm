#pragma once
#include "oreslang/runtime/NativeActorScheduler.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Four independent native carrier domains, not four threads. Each defaults to
 * five pthread carriers. Actor refs are domain-scoped capabilities.
 *
 * This is a host-side ownership and dispatch substrate, NOT a sandbox:
 * UNTRUSTED callbacks must route through an independently enforced sandbox
 * boundary. The VM cannot establish Graal polyglot isolation in native C.
 */
enum OresNativeVMDomain {
    ORES_NATIVE_VM_MAIN = 0,
    ORES_NATIVE_VM_SHARED = 1,
    ORES_NATIVE_VM_TRUSTED_PRIVATE = 2,
    ORES_NATIVE_VM_UNTRUSTED = 3,
    ORES_NATIVE_VM_DOMAIN_COUNT = 4
};

typedef struct {
    size_t actor_slots;       /* required; includes supervisor actor slots */
    size_t mailbox_capacity;  /* required */
    size_t carrier_workers;   /* 0 => 5 */
    OresNativeActorTaskFn on_message; /* host callback; required */
    OresNativeActorTaskFn on_control; /* optional priority CONTROL callback */
    /* Optional typed terminal-failure handler, serialized on parent's lane.
     * REQUIRED for supervised child construction in this domain; otherwise
     * spawn_supervised fails without consuming the requested actor slot.
     * Not an actor-local panic catch or an external supervisor pool. */
    OresNativeActorPanicFn on_failure;
    void* context;
} OresNativeVMDomainConfig;

typedef struct {
    OresNativeVMDomainConfig domains[ORES_NATIVE_VM_DOMAIN_COUNT];
} OresNativeVMConfig;

typedef struct OresNativeVM OresNativeVM;
OresNativeVM* ores_native_vm_open(const OresNativeVMConfig* config);
/* Opt-in quota/cpuset-aware carrier admission. An explicit nonzero
 * carrier_workers is preserved for its domain, while zero-valued domains
 * divide remaining detected CPU capacity (at least one pthread per domain).
 * This sizes pools, but does NOT pin them, provide isolation, or implement
 * region/Task ownership. Legacy open() retains its 0 => 5 contract.
 */
OresNativeVM* ores_native_vm_open_cpu_budgeted(const OresNativeVMConfig* config);
/* Separate opt-in strict cross-domain physical callback concurrency limit,
 * including under 4 CPUs. Leaves configured per-domain carrier counts
 * unchanged. A denied callback stays on its scheduler's ready queue and no
 * Oreslang actor code or Task is run while waiting for a permit.
 * Host-only; independently owned nb Task schedulers are not covered yet. */
OresNativeVM* ores_native_vm_open_with_physical_turn_limit(
    const OresNativeVMConfig* config, size_t maximum);
size_t ores_native_vm_active_physical_turns(const OresNativeVM* vm);
/* Host-detected snapshot, 1 minimum; not the count of exclusive CPU cores. */
size_t ores_native_vm_effective_cpu_budget(void);
size_t ores_native_vm_workers(const OresNativeVM* vm, enum OresNativeVMDomain domain);
/* Security admission probe, NOT a capability grant.
 * All four native scheduler domains are in-process host callbacks and have
 * zero independent VM heaps or OS address-space boundaries, even UNTRUSTED.
 * Return 0 for all current native domains and invalid refs. Production
 * UNTRUSTED source must run through an independently validated Graal isolate
 * or stronger externally sandboxed worker, never this callback alone.
 */
int ores_native_vm_has_hard_security_boundary(
    const OresNativeVM* vm, enum OresNativeVMDomain domain);
/* Number of physically started carriers, zero until first admitted work. */
size_t ores_native_vm_started_carriers(
    OresNativeVM* vm, enum OresNativeVMDomain domain);
OresNativeActorRef ores_native_vm_spawn(
    OresNativeVM* vm, enum OresNativeVMDomain domain, size_t actor_slot);
/* Trusted host-only immutable parentage. Child and supervisor must be distinct
 * live actors in the SAME VM domain. A child slot is single-use. No delegation
 * capability is granted to guest code, and no cross-domain parent is allowed.
 * Failed registration leaves the child slot unspawned. */
OresNativeActorRef ores_native_vm_spawn_supervised(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    size_t actor_slot, OresNativeActorRef supervisor);
/* Return immutable registered parent, or zero ref for missing, stale,
 * unsupervised or invalid child. Host metadata only. */
OresNativeActorRef ores_native_vm_supervisor_of(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef child);
/* Trusted host receipt after external actor-owned resource finalization.
 * Requires the exact child ActorRef and scheduler DONE, accepts only once.
 * Does not itself execute destructors or verify cleanup outside the VM.
 * Repeated/stale/unregistered/live child confirmations fail closed. */
int ores_native_vm_confirm_supervised_finalized(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef child);
/* Trusted runtime terminal notice, never a guest-authorized public syscall.
 * Requires exact registered child, same-domain supervisor, STOP + quiescence,
 * and an explicit host finalization receipt BEFORE publication. On a full
 * parent CONTROL slot, returns 0 without consuming the terminal notice:
 * caller owns retry/escalation. On success publishes exactly once; it does
 * not itself restart, reclaim, or invoke policy synchronously. */
int ores_native_vm_publish_supervised_failure(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorTerminalFailure failure);
OresNativeSendStatus ores_native_vm_try_send(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef actor, uint64_t payload);
/* Reject supervisor stop until all its registered direct children have
 * drained AND their terminal notices have been admitted to priority CONTROL.
 * VM-wide retirement still drains previously accepted actor work, but it
 * cannot guarantee undelivered terminal notices without host reconciliation. */
int ores_native_vm_stop(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
/* External trusted host barriers for compiler-generated linear ActorRef
 * stop/teardown. They do not implement source-level suspend or Future<T>.
 * Never call wait_done from a VM carrier callback. */
int ores_native_vm_poll_done(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
/* Retained immutable post-mortem for the exact domain-scoped ActorRef. */
int ores_native_vm_actor_terminal_state(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef actor, OresNativeActorTerminalState* output);
int ores_native_vm_wait_done(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
int ores_native_vm_publish_control(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef parent, uint64_t payload);

/* Metadata-only VM master CONTROL coordinator. Coalesces one host wake bit
 * per domain. The host event loop explicitly pumps it; no master pthread or
 * actor carrier starts and no guest callback runs on this path.
 * A wake is NOT actor CONTROL admission, receipt, or supervisor delivery:
 * use ores_native_vm_publish_control() for actual actor mailbox admission.
 * Producer/API calls must be externally fenced before final vm_close(). */
int ores_native_vm_control_notify(
    OresNativeVM* vm, enum OresNativeVMDomain domain);
unsigned ores_native_vm_control_pending(OresNativeVM* vm);
unsigned ores_native_vm_control_pump(OresNativeVM* vm);

/* Trusted host-only optional NIO backend ownership. Pure-C scheduler users
 * do not require C++ linkage; the separately compiled native NIO adapter
 * supplies a destructor callback. Attach once before guest admission.
 *
 * Every caller must externally fence VM producers before final close.
 * VM retirement blocks further backend access. The destructor may join
 * reactor threads and is invoked ONLY during external vm_close(), never from
 * a guest actor callback or on a reactor thread.
 */
typedef void (*OresNativeVMNioDestroy)(void* opaque_backend);
/* Bootstrap-only check; final attach repeats this under the CONTROL gate. */
int ores_native_vm_nio_can_attach(OresNativeVM* vm);
/* Atomically reserve NIO startup before any selector threads are created.
 * Actor spawn rejects while the reservation is held; always cancel it on
 * failed bootstrap, or consume it via attach_nio on success.
 */
int ores_native_vm_nio_begin_bootstrap(OresNativeVM* vm);
void ores_native_vm_nio_cancel_bootstrap(OresNativeVM* vm);
int ores_native_vm_attach_nio(OresNativeVM* vm, void* backend,
                              OresNativeVMNioDestroy destroy);
/* Nonblocking trusted-host pin. Every successful acquisition requires one
 * release, including when VM retirement begins during an operation.
 * External callers must fence new API entry before freeing the VM object.
 */
void* ores_native_vm_nio_acquire(OresNativeVM* vm);
void ores_native_vm_nio_release(OresNativeVM* vm, void* backend);

/* No new admissions after retire. Existing mail/control drains before destroy.
 * The caller MUST fence all producer/API calls before close and MUST NOT call
 * close from a carrier callback. Returns 0 if a carrier calls close (VM remains
 * retired, and an external host must retry); 1 after complete destruction.
 * No forceful termination of an uncooperative guest callback is attempted.
 */
void ores_native_vm_retire(OresNativeVM* vm);
int ores_native_vm_close(OresNativeVM* vm);
#ifdef __cplusplus
}
#endif
