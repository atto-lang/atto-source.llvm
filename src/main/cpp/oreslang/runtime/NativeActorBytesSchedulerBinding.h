#ifndef ORESLANG_NATIVE_ACTOR_BYTES_SCHEDULER_BINDING_H
#define ORESLANG_NATIVE_ACTOR_BYTES_SCHEDULER_BINDING_H

#include "oreslang/runtime/NativeActorBytesInbox.h"
#include "oreslang/runtime/NativeActorScheduler.h"

/* Narrow trusted-host binding, not source Bytes lowering. An actor's DATA
 * handler must authenticate against the actual scheduler-issued physical
 * ActorRef and admission sequence before reading its private byte queue. */
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef actor;
    OresNativeActorBytesInbox* inbox;
    OresNativeActorBytesRef storage;
} OresNativeActorBytesSchedulerBinding;

static inline int ores_native_actor_bytes_bind_scheduler(
    OresNativeActorBytesSchedulerBinding* out,
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    OresNativeActorBytesInbox* inbox) {
    if (!out || !scheduler || !inbox || !actor.scheduler_domain ||
        actor.actor_slot == UINT64_MAX || !actor.generation) return 0;
    OresNativeActorBytesRef storage = ores_native_actor_bytes_ref(inbox);
    if (storage.actor_id != actor.actor_slot + 1u ||
        storage.incarnation != actor.generation || !storage.nonce) return 0;
    *out = (OresNativeActorBytesSchedulerBinding){
        scheduler, actor, inbox, storage
    };
    return 1;
}

/* The native scheduler proves that the calling thread currently owns THIS
 * actor's running DATA turn. An equal actor ID on another carrier, CONTROL
 * callback, WAKE, external caller or stale ActorRef cannot pass.
 * This is still a separate byte queue and is NOT atomic with scheduler
 * DATA admission: callers must NOT use send+try_send as a Kafka broker
 * without a unified admission transaction or rollback/reconciliation. */
static inline int ores_native_actor_bytes_receive_current_data(
    const OresNativeActorBytesSchedulerBinding* binding,
    void* output, size_t capacity, size_t* required_length) {
    uint64_t sequence = 0;
    if (!binding || !ores_native_actor_scheduler_current_data_mail_sequence(
            binding->scheduler, binding->actor, &sequence) || !sequence)
        return ORES_ACTOR_BYTES_DENIED;
    return ores_native_actor_bytes_try_receive(
        binding->inbox, binding->storage, output, capacity, required_length);
}
#endif
