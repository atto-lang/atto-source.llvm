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
     * This is not an actor-local panic catch or an external supervisor pool. */
    OresNativeActorPanicFn on_failure;
    void* context;
} OresNativeVMDomainConfig;

typedef struct {
    OresNativeVMDomainConfig domains[ORES_NATIVE_VM_DOMAIN_COUNT];
} OresNativeVMConfig;

typedef struct OresNativeVM OresNativeVM;
OresNativeVM* ores_native_vm_open(const OresNativeVMConfig* config);
size_t ores_native_vm_workers(const OresNativeVM* vm, enum OresNativeVMDomain domain);
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
/* Trusted runtime terminal notice, never a guest-authorized public syscall.
 * Requires exact registered child, same-domain supervisor, STOP + quiescence,
 * and host-guaranteed resource finalization BEFORE publication. On a full
 * parent CONTROL slot, returns 0 without consuming the terminal notice:
 * caller owns retry/escalation. On success publishes exactly once; it does
 * not itself restart, reclaim, or invoke policy synchronously. */
int ores_native_vm_publish_supervised_failure(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorTerminalFailure failure);
OresNativeSendStatus ores_native_vm_try_send(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef actor, uint64_t payload);
int ores_native_vm_stop(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
/* External trusted host barriers for compiler-generated linear ActorRef
 * stop/teardown. They do not implement source-level suspend or Future<T>.
 * Never call wait_done from a VM carrier callback. */
int ores_native_vm_poll_done(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
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
