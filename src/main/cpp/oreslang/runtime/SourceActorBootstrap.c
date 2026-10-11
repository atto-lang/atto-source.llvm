#include "oreslang/runtime/SourceActorBootstrap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeActorFuture.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOURCE_ACTOR_MAGIC UINT64_C(0x4f52455353504157)
#define SOURCE_GROUP_SLOTS 64u
#define SOURCE_GROUP_CARRIERS 4u
#define SOURCE_GROUP_MAILBOX 128u
#define SOURCE_GROUP_ROOT_SLOT (SOURCE_GROUP_SLOTS - 1u)

#define SOURCE_MAX_VARIANTS 64u
#define SOURCE_PRODUCER_CLOSED (UINT64_C(1) << 63)
#define SOURCE_PRODUCER_COUNT (SOURCE_PRODUCER_CLOSED - 1)

typedef struct {
    OresMp2Contract value;
    char* owned_schema;
    char* owned_shape;
    uint8_t sha[32];
    char* owned_enum_name;
    uint32_t declared_tag;
} SourceVariantContract;

typedef struct {
    OresMp2Owned packet;
    size_t variant_index;
} SourceWireDelivery;

typedef struct {
    uint64_t magic;
    uint64_t id;
    OresActorI64Heap* heap;
    OresNativeActorRef ref;
    OresSourceActorInit constructor;
    OresSourceActorMessage on_message;
    OresSourceActorWireMessage on_wire;
    OresSourceActorVariantMessage on_variant;
    SourceVariantContract* variants;
    size_t variant_count;
    atomic_size_t wire_reserved;
    atomic_uint_fast64_t producer_gate;
    atomic_int initialized;
    atomic_uint_fast64_t processed;
    atomic_int_fast64_t last_result;
} SourceActor;

typedef struct {
    pthread_cond_t completed;
    OresNativeActorFuture* ready_future;
    OresSourceActorContinuation continuation;
    SourceActor* actor;
    int done;
    int64_t result;
} ReadyAwaitFrame;

typedef struct {
    pthread_mutex_t mutex;
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef root_ref;
    ReadyAwaitFrame* pending_ready;
    SourceActor* actors[SOURCE_GROUP_SLOTS];
    size_t next_slot;
    size_t live;
    size_t peak;
} SourceActorGroup;

/* Process-local trusted-main group; the compiler only supports linear refs
 * in a zero-argument top-level main. There is exactly one worker pool per
 * active group, not one pool per actor.
 */
static SourceActorGroup group = { .mutex = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uint64_t next_actor_id = 4100;
static void check_invariant(int condition, const char* predicate, int line) {
    if (!condition) {
        fprintf(stderr, "SOURCE_ACTOR_INVARIANT_FAILURE line=%d check=%s\n",
                line, predicate);
        abort();
    }
}
#define ensure(expr) check_invariant((expr), #expr, __LINE__)
static void group_lock(void) { ensure(pthread_mutex_lock(&group.mutex) == 0); }
static void group_unlock(void) { ensure(pthread_mutex_unlock(&group.mutex) == 0); }

static SourceActor* lookup(size_t index) {
    ensure(index < SOURCE_GROUP_SLOTS);
    SourceActor* actor = group.actors[index];
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC);
    return actor;
}
static void initialize(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    if (slot == SOURCE_GROUP_ROOT_SLOT) {
        /* Root continuation is admitted as its own exclusive CONTROL turn.
         * It cannot run on the child constructor's carrier inline. */
        ensure(payload == 1);
        group_lock();
        ReadyAwaitFrame* frame = group.pending_ready;
        ensure(frame && !frame->done && frame->ready_future);
        ensure(ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_READY);
        OresSourceActorContinuation continuation = frame->continuation;
        SourceActor* actor = frame->actor;
        group_unlock();
        const int64_t result = continuation(actor);
        group_lock();
        frame->result = result;
        frame->done = 1;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fputs("SOURCE_AWAIT_READY_CONTINUATION_PASS root_lane=1 real_future=1\n",
            stderr);
#endif
        ensure(pthread_cond_signal(&frame->completed) == 0);
        group_unlock();
        return;
    }
    ensure(payload == 0);
    SourceActor* instance = lookup(slot);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    instance->constructor(instance->heap, turn.domain, turn.epoch);
    ores_actor_i64_heap_leave(instance->heap, turn);
    atomic_store_explicit(&instance->initialized, 1, memory_order_release);
    group_lock();
    /* Registration may race with initialization. Both paths serialize via
     * group.mutex and settle exactly once through NativeActorFuture. */
    ReadyAwaitFrame* frame = group.pending_ready;
    if (frame && frame->actor == instance
        && ores_native_actor_future_status(frame->ready_future)
            == ORES_NATIVE_FUTURE_PENDING) {
        /* Registration can observe initialized=1 before the initializer
         * acquires group.mutex. Only the still-pending Future gets settled;
         * an already-published READY wake must never be completed twice. */
        ensure(ores_native_actor_future_complete(
            frame->ready_future, ORES_NATIVE_FUTURE_READY));
    }
    group_unlock();
}
static void deliver(void* raw, size_t slot, uint64_t payload) {
    ensure(raw == &group);
    SourceActor* instance = lookup(slot);
    ensure((instance->on_message != NULL)
        + (instance->on_wire != NULL) + (instance->on_variant != NULL) == 1);
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    OresActorTurn turn = ores_actor_i64_heap_enter(instance->heap, instance->id, 1);
    int64_t result;
    if (instance->on_wire || instance->on_variant) {
        SourceWireDelivery* accepted = (SourceWireDelivery*)(uintptr_t)payload;
        ensure(accepted && accepted->packet.bytes && accepted->packet.size);
        ensure(accepted->variant_index < instance->variant_count);
        if (instance->on_variant) {
            result = instance->on_variant(instance->heap, turn.domain, turn.epoch,
                    accepted->variant_index, accepted->packet.bytes,
                    accepted->packet.size);
        } else {
            ensure(accepted->variant_index == 0 && instance->variant_count == 1);
            result = instance->on_wire(instance->heap, turn.domain, turn.epoch,
                    accepted->packet.bytes, accepted->packet.size);
        }
        ores_actor_i64_heap_leave(instance->heap, turn);
        ores_mp2_owned_release(&accepted->packet);
        free(accepted);
        size_t before = atomic_fetch_sub_explicit(
            &instance->wire_reserved, 1, memory_order_acq_rel);
        ensure(before > 0);
    } else {
        result = instance->on_message(instance->heap, turn.domain,
                                      turn.epoch, (int64_t)payload);
        ores_actor_i64_heap_leave(instance->heap, turn);
    }
    atomic_store_explicit(&instance->last_result, result, memory_order_relaxed);
    atomic_fetch_add_explicit(&instance->processed, 1, memory_order_release);
}
/* Compiler contracts are stable for the duration of the construction
 * call, but not necessarily afterwards. Validate only our own immutable
 * snapshot, not borrowed fields that can change after an initial check.
 */
static size_t source_bounded_length(const char* string, size_t max_length) {
    if (!string) return max_length + 1;
    for (size_t i = 0; i <= max_length; ++i)
        if (string[i] == '\0') return i;
    return max_length + 1;
}
static void release_source_variants(SourceVariantContract* variants,
                                    size_t count) {
    if (!variants) return;
    for (size_t i = 0; i < count; ++i) {
        free(variants[i].owned_schema);
        free(variants[i].owned_shape);
        free(variants[i].owned_enum_name);
        memset(variants[i].sha, 0, sizeof(variants[i].sha));
    }
    free(variants);
}

/* Compiler-generated domains are immutable and complete. Copy-before-check
 * prevents a mutable host descriptor from being validated under one shape
 * and registered under another. Duplicate schema IDs are ambiguous and
 * always rejected rather than matched by payload or declaration order. */
static SourceVariantContract* source_copy_variants(
    const OresMp2Contract* contracts, size_t count,
    const OresSourceNominalCase* nominal) {
    if (!contracts || count == 0 || count > SOURCE_MAX_VARIANTS) return NULL;
    SourceVariantContract* owned = calloc(count, sizeof(*owned));
    if (!owned) return NULL;
    for (size_t i = 0; i < count; ++i) {
        const OresMp2Contract* src = &contracts[i];
        if (!src->schema_id || !src->shape || !src->shape_sha256) goto fail;
        size_t slen = source_bounded_length(src->schema_id, 256);
        size_t tlen = source_bounded_length(src->shape, 4096);
        if (!slen || slen > 256 || !tlen || tlen > 4096) goto fail;
        owned[i].owned_schema = malloc(slen + 1);
        owned[i].owned_shape = malloc(tlen + 1);
        if (!owned[i].owned_schema || !owned[i].owned_shape) goto fail;
        memcpy(owned[i].owned_schema, src->schema_id, slen + 1);
        memcpy(owned[i].owned_shape, src->shape, tlen + 1);
        memcpy(owned[i].sha, src->shape_sha256, sizeof(owned[i].sha));
        owned[i].value = (OresMp2Contract){
            owned[i].owned_schema, owned[i].owned_shape, owned[i].sha
        };
        if (!ores_mp2_contract_valid(&owned[i].value)) goto fail;
        /* A Java/Oreslang typed enum schema is NOMINAL. The structural
         * registration entrypoint cannot guarantee its embedded enum owner
         * and tag. Require the pinned-tag API, never silently degrade it. */
        if (!nominal && strncmp(owned[i].value.schema_id,
                               "oreslang.enum.",
                               sizeof("oreslang.enum.") - 1) == 0)
            goto fail;
        if (nominal) {
            if (!nominal[i].enum_name || !nominal[i].declared_tag
                    || nominal[i].declared_tag > INT32_MAX
                    || strncmp(owned[i].value.shape, "T[S,I", 5) != 0
                    || (owned[i].value.shape[5] != ']'
                        && owned[i].value.shape[5] != ','))
                goto fail;
            size_t nlen = source_bounded_length(nominal[i].enum_name, 256);
            if (!nlen || nlen > 256) goto fail;
            /* Source enum owners are compiler identifiers, not arbitrary
             * runtime strings chosen by serialized wire payloads. */
            for (size_t j = 0; j < nlen; ++j) {
                const unsigned char ch = (unsigned char)nominal[i].enum_name[j];
                if (!((ch >= 'A' && ch <= 'Z')
                      || (ch >= 'a' && ch <= 'z')
                      || ch == '_' || (j > 0 && ch >= '0' && ch <= '9')))
                    goto fail;
            }
            owned[i].owned_enum_name = malloc(nlen + 1);
            if (!owned[i].owned_enum_name) goto fail;
            memcpy(owned[i].owned_enum_name, nominal[i].enum_name, nlen + 1);
            owned[i].declared_tag = nominal[i].declared_tag;
        }
        for (size_t earlier = 0; earlier < i; ++earlier) {
            if (strcmp(owned[earlier].value.schema_id,
                       owned[i].value.schema_id) == 0) goto fail;
        }
    }
    return owned;
fail:
    release_source_variants(owned, count);
    return NULL;
}
/* Returns zero only for invalid trusted registration. Configuration and
 * teardown are single-owner operations; concurrent host senders must be
 * fenced before stop to avoid use-after-free of the opaque actor handle.
 */
static void* source_spawn(
    OresSourceActorInit constructor, OresSourceActorMessage on_message,
    OresSourceActorWireMessage on_wire,
    OresSourceActorVariantMessage on_variant, uint64_t fields,
    const OresMp2Contract* contracts, size_t variant_count,
    const OresSourceNominalCase* nominal) {
    ensure(constructor != NULL && fields >= 1 && fields <= 4096);
    ensure((on_message != NULL) + (on_wire != NULL)
        + (on_variant != NULL) == 1);
    SourceVariantContract* registered = NULL;
    if (on_wire || on_variant) {
        registered = source_copy_variants(contracts, variant_count, nominal);
        if (!registered) return NULL;
    }
    group_lock();
    if (!group.scheduler) {
        ensure(group.live == 0 && group.next_slot == 0 && group.peak == 0
            && group.pending_ready == NULL);
        group.scheduler = ores_native_actor_scheduler_open_control(
            SOURCE_GROUP_SLOTS, SOURCE_GROUP_MAILBOX, SOURCE_GROUP_CARRIERS,
            deliver, initialize, &group);
        ensure(group.scheduler != NULL);
        group.root_ref = ores_native_actor_scheduler_spawn(
            group.scheduler, SOURCE_GROUP_ROOT_SLOT);
        ensure(group.root_ref.scheduler_domain != 0);
    }
    ensure(group.next_slot < SOURCE_GROUP_ROOT_SLOT);
    const size_t slot = group.next_slot++;
    SourceActor* instance = (SourceActor*)calloc(1, sizeof(*instance));
    ensure(instance != NULL);
    instance->magic = SOURCE_ACTOR_MAGIC;
    instance->id = atomic_fetch_add_explicit(&next_actor_id, 1, memory_order_relaxed);
    ensure(instance->id != 0 && instance->id != UINT64_MAX);
    instance->constructor = constructor;
    instance->on_message = on_message;
    instance->on_wire = on_wire;
    instance->on_variant = on_variant;
    instance->variants = registered;
    instance->variant_count = registered ? variant_count : 0;
    atomic_init(&instance->wire_reserved, 0);
    atomic_init(&instance->producer_gate, 0);
    instance->heap = ores_actor_i64_heap_open(instance->id, 1, fields);
    group.actors[slot] = instance;
    instance->ref = ores_native_actor_scheduler_spawn(group.scheduler, slot);
    ensure(instance->ref.scheduler_domain && instance->ref.actor_slot == slot);
    group.live++;
    if (group.live > group.peak) group.peak = group.live;
    /* CONTROL initializes the actual compiler-emitted actor constructor
     * ahead of normal messages, without blocking the submitting carrier. */
    ensure(ores_native_actor_scheduler_publish_control(
        group.scheduler, instance->ref, 0));
    group_unlock();
    return instance;
}
void* oreslang_source_actor_spawn(OresSourceActorInit constructor,
                                  OresSourceActorMessage on_message,
                                  uint64_t fields) {
    return source_spawn(constructor, on_message, NULL, NULL,
                        fields, NULL, 0, NULL);
}
void* oreslang_source_actor_spawn_wire(
    OresSourceActorInit constructor, OresSourceActorWireMessage on_wire,
    uint64_t fields, const OresMp2Contract* contract) {
    if (!on_wire || !contract) return NULL;
    return source_spawn(constructor, NULL, on_wire, NULL,
                        fields, contract, 1, NULL);
}
void* oreslang_source_actor_spawn_variants(
    OresSourceActorInit constructor,
    OresSourceActorVariantMessage on_variant, uint64_t fields,
    const OresMp2Contract* contracts, size_t contract_count) {
    if (!on_variant) return NULL;
    return source_spawn(constructor, NULL, NULL, on_variant,
                        fields, contracts, contract_count, NULL);
}
void* oreslang_source_actor_spawn_tagged_variants(
    OresSourceActorInit constructor, OresSourceActorVariantMessage on_variant,
    uint64_t fields, const OresMp2Contract* contracts,
    const OresSourceNominalCase* identities, size_t contract_count) {
    if (!on_variant || !identities) return NULL;
    return source_spawn(constructor, NULL, NULL, on_variant,
                        fields, contracts, contract_count, identities);
}
int64_t oreslang_source_actor_await_ready(
    void* opaque, OresSourceActorContinuation continuation) {
    SourceActor* actor = (SourceActor*)opaque;
    ensure(actor && actor->magic == SOURCE_ACTOR_MAGIC && continuation);
    group_lock();
    ensure(group.scheduler && !group.pending_ready);
    const size_t slot = (size_t)actor->ref.actor_slot;
    ensure(slot < group.next_slot && group.actors[slot] == actor);
    ReadyAwaitFrame frame = {0};
    ensure(pthread_cond_init(&frame.completed, NULL) == 0);
    frame.actor = actor;
    frame.continuation = continuation;
    frame.ready_future = ores_native_actor_future_open(
        group.scheduler, group.root_ref, 1);
    ensure(frame.ready_future != NULL);
    group.pending_ready = &frame;
    if (atomic_load_explicit(&actor->initialized, memory_order_acquire)) {
        ensure(ores_native_actor_future_complete(
            frame.ready_future, ORES_NATIVE_FUTURE_READY));
    }
    /* External entrypoint barrier only. This caller is never a scheduler
     * worker; pending Future waits consume no ActorGroup carrier thread. */
    while (!frame.done) {
        ensure(pthread_cond_wait(&frame.completed, &group.mutex) == 0);
    }
    ensure(group.pending_ready == &frame);
    group.pending_ready = NULL;
    const int64_t result = frame.result;
    ores_native_actor_future_destroy(frame.ready_future);
    ensure(pthread_cond_destroy(&frame.completed) == 0);
    group_unlock();
    return result;
}
void oreslang_source_actor_send(void* opaque, int64_t payload) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC
        && instance->on_message && !instance->on_wire);
    /* The source language still only permits one trusted-main submitter.
     * Nonblocking bounded sends fail closed on overload. */
    ensure(ores_native_actor_scheduler_send(
        group.scheduler, instance->ref, (uint64_t)payload));
}
/* Peek the *canonical* schema ONLY from a detached owned wire snapshot.
 * The selected contract then validates those exact same bytes including
 * version, SHA and recursive payload shape; never touch caller memory twice.
 */
static int source_select_variant(const SourceActor* actor,
                                 const uint8_t* wire, size_t size,
                                 size_t* selected) {
    if (!wire || size < 5 || wire[0] != 0x94 || wire[1] != 2) return 0;
    size_t pos = 3;
    size_t schema_bytes;
    uint8_t head = wire[2];
    if ((head & 0xe0) == 0xa0) {
        schema_bytes = (size_t)(head & 31);
    } else if (head == 0xd9) {
        if (pos >= size) return 0;
        schema_bytes = wire[pos++];
        if (schema_bytes <= 31) return 0;
    } else if (head == 0xda) {
        if (size - pos < 2) return 0;
        schema_bytes = ((size_t)wire[pos] << 8) | wire[pos+1];
        pos += 2;
        if (schema_bytes <= 255) return 0;
    } else return 0;
    if (schema_bytes == 0 || schema_bytes > 256
            || schema_bytes > size - pos) return 0;
    for (size_t i = 0; i < actor->variant_count; ++i) {
        const char* name = actor->variants[i].value.schema_id;
        if (strlen(name) == schema_bytes
                && memcmp(name, wire + pos, schema_bytes) == 0) {
            *selected = i;
            return 1;
        }
    }
    return 0;
}
/* Producer leases protect calls that have entered the send ABI before
 * trusted stop closes admission. They do NOT validate stale opaque pointers:
 * the host must fence the start of new producer calls before destruction.
 * No hot-path clock reads and no per-message polling.
 */
static int source_enter_producer(SourceActor* actor) {
    uint_fast64_t state = atomic_load_explicit(&actor->producer_gate,
                                               memory_order_acquire);
    do {
        if ((state & SOURCE_PRODUCER_CLOSED)
                || (state & SOURCE_PRODUCER_COUNT) == SOURCE_PRODUCER_COUNT)
            return 0;
    } while (!atomic_compare_exchange_weak_explicit(
        &actor->producer_gate, &state, state + 1,
        memory_order_acq_rel, memory_order_acquire));
    return 1;
}
static void source_leave_producer(SourceActor* actor) {
    uint_fast64_t prior = atomic_fetch_sub_explicit(
        &actor->producer_gate, 1, memory_order_acq_rel);
    ensure((prior & SOURCE_PRODUCER_COUNT) != 0);
}
static int source_send_binary_unlocked(SourceActor* actor,
                                       const uint8_t* wire, size_t size);
static int source_send_binary(SourceActor* actor,
                              const uint8_t* wire, size_t size) {
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC || !source_enter_producer(actor))
        return 0;
    int result = source_send_binary_unlocked(actor, wire, size);
    source_leave_producer(actor);
    return result;
}
static int source_send_binary_unlocked(SourceActor* actor,
                              const uint8_t* wire, size_t size) {
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC
            || (!actor->on_wire && !actor->on_variant) || !wire
            || size > ORES_MP2_MAX_BYTES) return 0;
    /* Every variant shares one admission counter and one actor scheduler
     * lane. Reserve BEFORE snapshot allocation or recursive typed parsing. */
    size_t pending = atomic_load_explicit(&actor->wire_reserved,
                                          memory_order_relaxed);
    do {
        if (pending >= SOURCE_GROUP_MAILBOX) return 0;
    } while (!atomic_compare_exchange_weak_explicit(
        &actor->wire_reserved, &pending, pending + 1,
        memory_order_acq_rel, memory_order_relaxed));
    /* Snapshot once BEFORE consulting the nominal schema ID. Previously
     * selection parsed caller-owned wire bytes before the codec copied
     * them, leading to two different reads of a mutable sender buffer.
     * The decoder now consumes only this identical pinned snapshot. */
    SourceWireDelivery* accepted = calloc(1, sizeof(*accepted));
    if (!accepted) {
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    if (size < 40 || size > ORES_MP2_MAX_BYTES) {
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    accepted->packet.bytes = malloc(size);
    if (!accepted->packet.bytes) {
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    accepted->packet.size = size;
    memcpy(accepted->packet.bytes, wire, size);
    size_t index = 0;
    if (!source_select_variant(actor, accepted->packet.bytes, size, &index)
            || !ores_mp2_validate_owned(&actor->variants[index].value,
                                         &accepted->packet)
            || (actor->variants[index].owned_enum_name
                && !ores_mp2_tagged_identity_matches_validated(
                    &actor->variants[index].value, &accepted->packet,
                    actor->variants[index].owned_enum_name,
                    actor->variants[index].declared_tag))) {
        ores_mp2_owned_release(&accepted->packet);
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    accepted->variant_index = index;
    if (!ores_native_actor_scheduler_send(group.scheduler, actor->ref,
                                           (uint64_t)(uintptr_t)accepted)) {
        ores_mp2_owned_release(&accepted->packet);
        free(accepted);
        atomic_fetch_sub_explicit(&actor->wire_reserved, 1,
                                  memory_order_acq_rel);
        return 0;
    }
    return 1;
}
int oreslang_source_actor_send_wire(void* opaque,
                                   const uint8_t* wire, size_t size) {
    SourceActor* actor = opaque;
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC
            || !actor->on_wire || actor->variant_count != 1) return 0;
    return source_send_binary(actor, wire, size);
}
int oreslang_source_actor_send_variant(void* opaque,
                                      const uint8_t* wire, size_t size) {
    SourceActor* actor = opaque;
    if (!actor || actor->magic != SOURCE_ACTOR_MAGIC || !actor->on_variant)
        return 0;
    return source_send_binary(actor, wire, size);
}
void oreslang_source_actor_stop(void* opaque) {
    SourceActor* instance = (SourceActor*)opaque;
    ensure(instance && instance->magic == SOURCE_ACTOR_MAGIC);
    group_lock();
    const size_t slot = (size_t)instance->ref.actor_slot;
    ensure(group.scheduler && slot < group.next_slot && group.actors[slot] == instance);
    /* Once this bit is set no new sender lease can enter. Producers that
     * already entered must finish their validation/enqueue or reject paths
     * before actor-owned contracts can be freed. */
    uint_fast64_t old_gate = atomic_fetch_or_explicit(
        &instance->producer_gate, SOURCE_PRODUCER_CLOSED,
        memory_order_acq_rel);
    ensure((old_gate & SOURCE_PRODUCER_CLOSED) == 0);
    ensure(ores_native_actor_scheduler_stop(group.scheduler, instance->ref));
    /* Never hold the ActorGroup mutex during a blocking host join.
     * A finishing initializer itself needs that mutex to publish readiness.
     * Trusted main is the sole source stop owner in this compiler subset. */
    group_unlock();
    /* Shutdown-only barrier, not cooperative polling inside actor execution.
     * The caller is the trusted external host, never a scheduler carrier. */
    while ((atomic_load_explicit(&instance->producer_gate,
                                memory_order_acquire) & SOURCE_PRODUCER_COUNT) != 0) {
        sched_yield();
    }
    ensure(ores_native_actor_scheduler_wait_done(group.scheduler, instance->ref));
    group_lock();
    ensure(group.actors[slot] == instance);
    ensure(atomic_load_explicit(&instance->initialized, memory_order_acquire) == 1);
    ensure(ores_native_actor_scheduler_poll_done(group.scheduler, instance->ref));
    ores_actor_i64_heap_retire(instance->heap);
    ensure(ores_actor_i64_heap_try_destroy(instance->heap));
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    fprintf(stderr,
        "SOURCE_ACTOR_SPAWN_STOP_VERIFIED constructor_on_worker=1 released=1 processed=%llu last=%lld\n",
        (unsigned long long)atomic_load_explicit(&instance->processed, memory_order_acquire),
        (long long)atomic_load_explicit(&instance->last_result, memory_order_relaxed));
#endif
    group.actors[slot] = NULL;
    ensure(atomic_load_explicit(&instance->wire_reserved,
                                 memory_order_acquire) == 0);
    release_source_variants(instance->variants, instance->variant_count);
    instance->magic = 0;
    free(instance);
    ensure(group.live > 0);
    group.live--;
    if (group.live == 0) {
        ensure(group.pending_ready == NULL);
        /* The root also lives on the shared pool, with an exclusive lane. */
        ensure(ores_native_actor_scheduler_stop(group.scheduler, group.root_ref));
        ensure(ores_native_actor_scheduler_wait_done(group.scheduler, group.root_ref));
        /* All children and root continuations are quiescent. */
        ores_native_actor_scheduler_retire(group.scheduler);
        ensure(ores_native_actor_scheduler_try_destroy(group.scheduler));
        group.scheduler = NULL;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
        fprintf(stderr,
            "SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=%zu peak=%zu carriers=%u\n",
            group.next_slot, group.peak, SOURCE_GROUP_CARRIERS);
#endif
        group.next_slot = 0;
        group.peak = 0;
    }
    group_unlock();
}
