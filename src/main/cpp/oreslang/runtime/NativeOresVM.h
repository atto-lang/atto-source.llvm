#pragma once
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeActorFuture.h"
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

/* ABI-separate optional continuation wake configuration. Keeping this out of
 * OresNativeVMDomainConfig preserves the existing public C struct layout for
 * hosts compiled against the original VM ABI. The wake callback reuses each
 * domain's existing context pointer from OresNativeVMConfig. */
typedef struct {
    OresNativeActorWakeFn domains[ORES_NATIVE_VM_DOMAIN_COUNT];
} OresNativeVMWakeConfig;

typedef struct OresNativeVM OresNativeVM;
OresNativeVM* ores_native_vm_open(const OresNativeVMConfig* config);
/* Opt-in wake-capable VM construction for continuation/NIO runtimes. Existing
 * ores_native_vm_open() remains wake-free and ABI-compatible. */
OresNativeVM* ores_native_vm_open_with_wakes(
    const OresNativeVMConfig* config, const OresNativeVMWakeConfig* wakes);
/* RT bootstrap: prestart every configured carrier in all four domains before
 * exposing the VM to guest work. No carrier will be created on its DATA or
 * CONTROL hot path thereafter. Unlike normal open(), this starts pthreads.
 * Only the trusted host may invoke this before guest admission; it is not a
 * hard-real-time certification or proof of bounded OS scheduling latency.
 */
OresNativeVM* ores_native_vm_open_realtime(const OresNativeVMConfig* config);
/* Equivalent post-open preparation for still-private, completely cold VMs.
 * A failed preparation must be followed by external-host VM teardown.
 */
int ores_native_vm_prepare_realtime(OresNativeVM* vm);
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
/* Source-style stop: reject new DATA and abandon queued DATA while preserving
 * an already-active turn and lifecycle CONTROL. */
int ores_native_vm_stop_sealed(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
/* Trusted runtime physical-resource loan for the exact VM ActorRef. A retain
 * that wins before STOP delays poll_done()/wait_done(); STOP winning first
 * rejects the retain. Release is valid after STOP and is generation-fenced.
 * These are runtime plumbing hooks, not guest-callable ActorRef methods. */
int ores_native_vm_actor_resource_retain(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
int ores_native_vm_actor_resource_release(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
/* Trusted runtime continuation-wake escrow. Source guest code cannot mint
 * these tickets. Reserve must happen before an external operation can suspend;
 * publish schedules the configured on_wake callback and never runs guest code
 * inline on the producer/completion thread. This VM slice accepts only targets
 * in the producer's exact scheduler domain; cross-scheduler continuation
 * routing requires a separately authenticated bridge. */
int ores_native_vm_actor_reserve_wake(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef producer, OresNativeActorRef target,
    uint64_t continuation_slot, uint64_t continuation_generation,
    uint64_t code_generation, uint64_t descriptor_generation,
    OresNativeWakeTicket* out);
int ores_native_vm_actor_transfer_wake(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeWakeTicket ticket);
int ores_native_vm_actor_cancel_wake(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeWakeTicket ticket);
int ores_native_vm_actor_abort_transferred_wake(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeWakeTicket ticket);
OresNativeWakePublishStatus ores_native_vm_actor_publish_wake(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeWakeTicket ticket, OresNativeWakeOutcome outcome);
int ores_native_vm_actor_is_current_wake(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeWakeTicket ticket);
size_t ores_native_vm_actor_outstanding_wake_loans(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef producer);
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

/*
 * Trusted continuation bridge helpers. The VM keeps its domain scheduler
 * private while allowing runtime-owned Future and logical-execution plumbing.
 * current_execution_context succeeds only from the selected domain's active
 * carrier callback; it never exposes the scheduler pointer to guest code.
 */
OresNativeActorFuture* ores_native_vm_actor_future_open(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef owner, uint64_t control_token);
int ores_native_vm_current_execution_context(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeExecutionContext* out);

/* VM-owned CONTROL wake coordinator. Legacy hosts can explicitly pump it
 * with zero additional threads; a trusted source bootstrap can prestart
 * exactly ONE dedicated blocking main CONTROL pthread before any actor spawn
 * and before prepare_realtime(). A real-time prestart attempt permanently
 * rejects late CONTROL-thread creation, even after failed preallocation.
 * To use both services, start CONTROL first and then prestart RT carriers.
 * Its only job is to coalesce signals to domain scheduler conditions:
 * it never executes guest code, delivers actor mailbox messages or becomes
 * a fourth actor worker pool. The three actor pools stay lazy.
 *
 * This is a wake, NOT actor CONTROL admission or a supervisor receipt.
 * Actual actor delivery still requires ores_native_vm_publish_control().
 * External hosts MUST fence producers before final vm_close().
 */
int ores_native_vm_start_main_control_loop(OresNativeVM* vm);
int ores_native_vm_main_control_loop_started(OresNativeVM* vm);
/* Saturating diagnostic count of successfully signalled pool wakeups.
 * Counts both autonomous and manual CONTROL pumps, not actor messages. */
uint64_t ores_native_vm_control_wakes_dispatched(OresNativeVM* vm);
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
