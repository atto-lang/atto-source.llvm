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

/* Compiled into the trusted runtime only. Binding is bootstrap-only and
 * cannot be partially published: the scheduler's lock excludes legacy DATA
 * while it makes an irrevocable (fail-closed) sequenced inbox transition. */
typedef struct {
    OresNativeActorBytesInbox* inbox;
    OresNativeActorBytesRef owner;
} OresNativeActorBytesBindContext;
static inline int ores_native_actor_bytes_bind_once(void* opaque) {
    OresNativeActorBytesBindContext* bind =
        (OresNativeActorBytesBindContext*)opaque;
    return bind && ores_native_actor_bytes_bind_sequenced(
        bind->inbox, bind->owner);
}
/* The definition follows the bind routine; its identity is registered
 * with the real scheduler to reject foreign commit callbacks. */
static inline int ores_native_actor_bytes_commit_payload(
    void* opaque, uint64_t sequence);

static inline int ores_native_actor_bytes_bind_scheduler(
    OresNativeActorBytesSchedulerBinding* out,
    OresNativeActorScheduler* scheduler, OresNativeActorRef actor,
    OresNativeActorBytesInbox* inbox) {
    if (!out || !scheduler || !inbox || !actor.scheduler_domain ||
        actor.actor_slot == UINT64_MAX || !actor.generation) return 0;
    OresNativeActorBytesRef storage = ores_native_actor_bytes_ref(inbox);
    if (storage.actor_id != actor.actor_slot + 1u ||
        storage.incarnation != actor.generation || !storage.nonce) return 0;
    /* Both halves are established under the scheduler lock: prior DATA
     * admissions or worker start reject the bind BEFORE touching storage.
     * The two-state transition then has no fallible scheduler step. */
    OresNativeActorBytesBindContext bind = {inbox, storage};
    if (!ores_native_actor_scheduler_bind_payload_only(
            scheduler, actor, ores_native_actor_bytes_commit_payload,
            ores_native_actor_bytes_bind_once, &bind)) return 0;
    *out = (OresNativeActorBytesSchedulerBinding){
        scheduler, actor, inbox, storage
    };
    return 1;
}

/* Scheduler -> inbox is the ONLY permitted lock ordering. This
 * immediate borrowed source is copied to the target's private slab during
 * the scheduler's one atomic payload-plus-DATA admission transaction. */
typedef struct {
    const OresNativeActorBytesSchedulerBinding* binding;
    const void* bytes;
    size_t length;
} OresNativeActorBytesCommitContext;
static inline int ores_native_actor_bytes_commit_payload(
    void* opaque, uint64_t sequence) {
    const OresNativeActorBytesCommitContext* ctx =
        (const OresNativeActorBytesCommitContext*)opaque;
    return ores_native_actor_bytes_try_send_sequenced(
        ctx->binding->inbox, ctx->binding->storage,
        ctx->bytes, ctx->length, sequence) == ORES_ACTOR_BYTES_OK;
}
/* Trusted native scheduler admission ONLY, not user-callable source ABI.
 * Legacy inbox.try_send + scheduler.try_send is NOT an atomic substitute.
 * No DATA message or owned payload is admitted on a rejected result. */
static inline OresNativeSendStatus ores_native_actor_bytes_try_send_atomic(
    const OresNativeActorBytesSchedulerBinding* binding,
    uint64_t payload, const void* bytes, size_t length) {
    if (!binding || !binding->scheduler || !binding->inbox ||
        !binding->storage.nonce || (length && !bytes))
        return ORES_NATIVE_SEND_INVALID_REF;
    OresNativeActorBytesCommitContext commit = {binding, bytes, length};
    return ores_native_actor_scheduler_try_send_committed(
        binding->scheduler, binding->actor, payload,
        ores_native_actor_bytes_commit_payload, &commit);
}

/* The native scheduler proves that the calling thread currently owns THIS
 * actor's running DATA turn. An equal actor ID on another carrier, CONTROL
 * callback, WAKE, external caller or stale ActorRef cannot pass.
 * Binding.try_send_atomic publishes private Bytes and DATA together,
 * but source-level ActorMail<Bytes> is still unsupported. */
static inline int ores_native_actor_bytes_receive_current_data(
    const OresNativeActorBytesSchedulerBinding* binding,
    void* output, size_t capacity, size_t* required_length) {
    uint64_t sequence = 0;
    if (!binding || !ores_native_actor_scheduler_current_data_mail_sequence(
            binding->scheduler, binding->actor, &sequence) || !sequence)
        return ORES_ACTOR_BYTES_DENIED;
    return ores_native_actor_bytes_try_receive_sequenced(
        binding->inbox, binding->storage, sequence,
        output, capacity, required_length);
}
#endif
