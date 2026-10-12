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
OresNativeSendStatus ores_native_vm_try_send(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef actor, uint64_t payload);
int ores_native_vm_stop(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor);
int ores_native_vm_publish_control(
    OresNativeVM* vm, enum OresNativeVMDomain domain,
    OresNativeActorRef parent, uint64_t payload);

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
