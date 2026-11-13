#ifndef ORESLANG_NATIVE_ORES_VM_BYTES_OWNED_H
#define ORESLANG_NATIVE_ORES_VM_BYTES_OWNED_H

#include "oreslang/runtime/NativeOresVM.h"
#include "oreslang/runtime/NativeActorBytesSchedulerBinding.h"
#include <stdlib.h>

/* Opt-in trusted-runtime Bytes substrate. This is NOT guest-exposed ABI,
 * source ActorMail<Bytes> lowering, shared borrowing, network I/O or Kafka.
 * The VM owns the ENTIRE inbox lifetime; callers never obtain its address.
 * The only permitted send is a VM-fenced atomic DATA+Bytes transaction. */
typedef struct {
    OresNativeActorBytesInbox* inbox;
    OresNativeActorBytesSchedulerBinding binding;
} OresNativeVMOwnedBytes;

static inline int ores_native_vm_bytes_bind(
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    void* resource) {
    OresNativeVMOwnedBytes* owned=(OresNativeVMOwnedBytes*)resource;
    return ores_native_actor_bytes_bind_scheduler(
        &owned->binding,scheduler,actor,owned->inbox);
}

/* VM invokes this callback only after exact scheduler DONE. It must never
 * run from the actor callback/carrier; no user destructor is invoked here.
 * If a low-level atomic_flag is unexpectedly busy, retain ownership for
 * a later finalization retry instead of freeing a live slab. */
static inline int ores_native_vm_bytes_cleanup(void* resource) {
    OresNativeVMOwnedBytes* owned=(OresNativeVMOwnedBytes*)resource;
    const OresNativeActorBytesRef storage =
        ores_native_actor_bytes_ref(owned->inbox);
    int retired=ores_native_actor_bytes_try_retire(owned->inbox,storage);
    if (retired==ORES_ACTOR_BYTES_BUSY) return 0;
#ifdef ORESLANG_ACTOR_BYTES_TESTING
    if (!ores_native_actor_bytes_all_zero_for_test(owned->inbox)) return 0;
#endif
    if (ores_native_actor_bytes_try_destroy(owned->inbox,storage)
            !=ORES_ACTOR_BYTES_OK)
        return 0;
    free(owned);
    return 1;
}

/* ActorRef must already exist and have NO admitted user mail. Binding and
 * VM ownership publish as a single serialized cold-path transaction.
 * Allocate at admission, never on steady-state actor sends/receives. */
static inline int ores_native_vm_bytes_enable(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor,
    size_t message_slots, size_t max_payload_bytes, size_t byte_budget) {
    if (!vm || !actor.scheduler_domain || !actor.generation
        || actor.actor_slot==UINT64_MAX) return 0;
    OresNativeVMOwnedBytes* owned =
        (OresNativeVMOwnedBytes*)calloc(1,sizeof(*owned));
    if (!owned) return 0;
    owned->inbox=ores_native_actor_bytes_open(
        actor.actor_slot+1u,actor.generation,
        message_slots,max_payload_bytes,byte_budget);
    if (!owned->inbox) { free(owned); return 0; }
    if (!ores_native_vm_attach_owned(
            vm,domain,actor,owned,ores_native_vm_bytes_bind,
            ores_native_vm_bytes_cleanup)) {
        const OresNativeActorBytesRef ref =
            ores_native_actor_bytes_ref(owned->inbox);
        (void)ores_native_actor_bytes_try_retire(owned->inbox,ref);
        (void)ores_native_actor_bytes_try_destroy(owned->inbox,ref);
        free(owned);
        return 0;
    }
    return 1;
}
typedef struct {
    uint64_t payload;
    const void* bytes;
    size_t length;
} OresNativeVMBytesSendCall;
static inline int ores_native_vm_bytes_send_call(void* resource,void* opaque) {
    OresNativeVMOwnedBytes* owned=(OresNativeVMOwnedBytes*)resource;
    const OresNativeVMBytesSendCall* call =
        (const OresNativeVMBytesSendCall*)opaque;
    return (int)ores_native_actor_bytes_try_send_atomic(
        &owned->binding,call->payload,call->bytes,call->length);
}

/* VM master lock is try-lock only; BUSY maps to RESOURCE_EXHAUSTED and
 * the source payload remains caller-owned for an eventual async retry.
 * A matching STOP is still reported as TERMINATED by the native scheduler. */
static inline OresNativeSendStatus ores_native_vm_bytes_try_send(
    OresNativeVM* vm, enum OresNativeVMDomain domain, OresNativeActorRef actor,
    uint64_t payload, const void* bytes, size_t length) {
    OresNativeVMBytesSendCall call={payload,bytes,length};
    int send_status=ORES_NATIVE_SEND_INVALID_REF;
    int invoked=ores_native_vm_try_call_owned(
        vm,domain,actor,ores_native_vm_bytes_send_call,&call,&send_status);
    if (invoked<0) return ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
    if (!invoked) return ORES_NATIVE_SEND_INVALID_REF;
    return (OresNativeSendStatus)send_status;
}
typedef struct {
    void* output;
    size_t capacity;
    size_t* required_length;
} OresNativeVMBytesReadCall;
static inline int ores_native_vm_bytes_read_call(void* resource,void* opaque) {
    OresNativeVMOwnedBytes* owned=(OresNativeVMOwnedBytes*)resource;
    const OresNativeVMBytesReadCall* call =
        (const OresNativeVMBytesReadCall*)opaque;
    return ores_native_actor_bytes_receive_current_data(
        &owned->binding,call->output,call->capacity,call->required_length);
}
/* Physical actor DATA callback only. Never returns a slab pointer.
 * A source guest cannot forge this OS-carrier authority. */
static inline int ores_native_vm_bytes_receive_current(
    OresNativeVM* vm,enum OresNativeVMDomain domain,OresNativeActorRef actor,
    void* output,size_t capacity,size_t* required_length) {
    OresNativeVMBytesReadCall call={output,capacity,required_length};
    int read_status=ORES_ACTOR_BYTES_DENIED;
    if (!ores_native_vm_current_data_call_owned(
        vm,domain,actor,ores_native_vm_bytes_read_call,&call,&read_status))
        return ORES_ACTOR_BYTES_DENIED;
    return read_status;
}
/* Trusted host observation only; no raw private slabs are exposed. */
typedef struct {size_t* pending;size_t* used;} OresNativeVMBytesCountCall;
static inline int ores_native_vm_bytes_observe_call(void* resource,void* opaque) {
    OresNativeVMOwnedBytes* owned=(OresNativeVMOwnedBytes*)resource;
    OresNativeVMBytesCountCall* call=(OresNativeVMBytesCountCall*)opaque;
    return ores_native_actor_bytes_observe(
        owned->inbox,owned->binding.storage,call->pending,call->used);
}
static inline int ores_native_vm_bytes_observe(
    OresNativeVM* vm,enum OresNativeVMDomain domain,OresNativeActorRef actor,
    size_t* pending,size_t* used) {
    if (!pending || !used) return 0;
    OresNativeVMBytesCountCall call={pending,used};
    int status=0;
    return ores_native_vm_try_call_owned(
        vm,domain,actor,ores_native_vm_bytes_observe_call,&call,&status)==1
        && status==ORES_ACTOR_BYTES_OK;
}
#endif
