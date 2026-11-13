#include "oreslang/runtime/NativeActorBytesInbox.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ORES_ACTOR_BYTES_MAX_SLOTS 128u
#define ORES_ACTOR_BYTES_MAX_PAYLOAD 65536u
#define ORES_ACTOR_BYTES_MAX_SLAB (8u * 1024u * 1024u)

struct OresNativeActorBytesInbox {
    atomic_flag busy;
    OresNativeActorBytesRef owner;
    size_t capacity, max_payload, budget;
    size_t first, next, queued, used;
    size_t* lengths;
    uint64_t* mail_sequences; /* 0 means unsequenced legacy trusted-host mail */
    uint64_t last_mail_sequence; /* monotonic, never reuse across ring wrap */
    unsigned char* slab;
    int retired;
    int sequenced_only; /* forbids mixing legacy host DATA with scheduler DATA */
};
static _Atomic uint64_t next_nonce = 1;

static int acquire(OresNativeActorBytesInbox* b) {
    return !atomic_flag_test_and_set_explicit(&b->busy, memory_order_acquire);
}
static void release(OresNativeActorBytesInbox* b) {
    atomic_flag_clear_explicit(&b->busy, memory_order_release);
}
static int same(const OresNativeActorBytesInbox* b, OresNativeActorBytesRef ref) {
    return ref.actor_id && ref.incarnation && ref.nonce &&
        b->owner.actor_id == ref.actor_id &&
        b->owner.incarnation == ref.incarnation &&
        b->owner.nonce == ref.nonce;
}
/* Volatile writes prevent elision of sensitive message scrubbing at O2/LTO. */
static void scrub(void* memory, size_t length) {
    volatile unsigned char* p = (volatile unsigned char*)memory;
    for (size_t i = 0; i < length; ++i) p[i] = 0;
}

OresNativeActorBytesInbox* ores_native_actor_bytes_open(
    uint64_t actor_id, uint64_t incarnation, size_t slots,
    size_t max_message_bytes, size_t byte_budget) {
    if (!actor_id || !incarnation || !slots || slots > ORES_ACTOR_BYTES_MAX_SLOTS ||
        !max_message_bytes || max_message_bytes > ORES_ACTOR_BYTES_MAX_PAYLOAD ||
        max_message_bytes > SIZE_MAX / slots || !byte_budget ||
        slots * max_message_bytes > ORES_ACTOR_BYTES_MAX_SLAB ||
        byte_budget > slots * max_message_bytes ||
        byte_budget > ORES_ACTOR_BYTES_MAX_SLAB ||
        slots > SIZE_MAX / sizeof(size_t)) return NULL;
    OresNativeActorBytesInbox* b = (OresNativeActorBytesInbox*)calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->slab = (unsigned char*)calloc(slots, max_message_bytes);
    b->lengths = (size_t*)calloc(slots, sizeof(size_t));
    b->mail_sequences = (uint64_t*)calloc(slots, sizeof(uint64_t));
    if (!b->slab || !b->lengths || !b->mail_sequences) {
        free(b->mail_sequences); free(b->lengths); free(b->slab); free(b);
        return NULL;
    }
    uint64_t nonce = atomic_fetch_add_explicit(&next_nonce, 1, memory_order_relaxed);
    if (!nonce || nonce == UINT64_MAX) {
        scrub(b->slab, slots * max_message_bytes);
        free(b->mail_sequences); free(b->lengths); free(b->slab); free(b);
        return NULL;
    }
    atomic_flag_clear(&b->busy);
    b->owner = (OresNativeActorBytesRef){actor_id, incarnation, nonce};
    b->capacity = slots;
    b->max_payload = max_message_bytes;
    b->budget = byte_budget;
    return b;
}

OresNativeActorBytesRef ores_native_actor_bytes_ref(
    const OresNativeActorBytesInbox* b) {
    OresNativeActorBytesRef nil = {0, 0, 0};
    /* Only the trusted runtime may read/mint this reference at actor spawn.
     * Calling this during concurrent destruction violates external quiescence. */
    return b ? b->owner : nil;
}

int ores_native_actor_bytes_bind_sequenced(
    OresNativeActorBytesInbox* b, OresNativeActorBytesRef owner) {
    if (!b || !same(b, owner)) return 0;
    if (!acquire(b)) return 0;
    const int ok = !b->retired && !b->sequenced_only && !b->queued &&
        !b->used && !b->last_mail_sequence;
    if (ok) b->sequenced_only = 1;
    release(b);
    return ok;
}

/* 0 selects the legacy unsequenced trusted-host interface. A committed
 * sequence comes only from the scheduler's source-mail admission ordinal. */
static int send_internal(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef target, const void* bytes, size_t length,
    uint64_t sequence) {
    if (!b || !same(b, target) || length > b->max_payload ||
        (length && !bytes)) return ORES_ACTOR_BYTES_DENIED;
    if (!acquire(b)) return ORES_ACTOR_BYTES_BUSY;
    int result = ORES_ACTOR_BYTES_DENIED;
    if (!b->retired && b->queued < b->capacity &&
        length <= b->budget - b->used &&
        ((sequence && b->sequenced_only &&
          sequence > b->last_mail_sequence) ||
         (!sequence && !b->sequenced_only))) {
        size_t index = b->next;
        unsigned char* target_slot = b->slab + index * b->max_payload;
        if (length) memmove(target_slot, bytes, length);
        b->lengths[index] = length;
        b->mail_sequences[index] = sequence;
        if (sequence) b->last_mail_sequence = sequence;
        b->next = index + 1 == b->capacity ? 0 : index + 1;
        b->queued++;
        b->used += length;
        result = ORES_ACTOR_BYTES_OK;
    }
    release(b);
    return result;
}
int ores_native_actor_bytes_try_send(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef target, const void* bytes, size_t length) {
    return send_internal(b, target, bytes, length, 0);
}
int ores_native_actor_bytes_try_send_sequenced(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef target, const void* bytes, size_t length,
    uint64_t source_mail_sequence) {
    if (!source_mail_sequence) return ORES_ACTOR_BYTES_DENIED;
    return send_internal(b, target, bytes, length, source_mail_sequence);
}
static int receive_internal(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef receiver, uint64_t sequence,
    void* output, size_t capacity, size_t* required_length) {
    if (!b || !same(b, receiver) || !required_length)
        return ORES_ACTOR_BYTES_DENIED;
    if (!acquire(b)) return ORES_ACTOR_BYTES_BUSY;
    int result = ORES_ACTOR_BYTES_DENIED;
    if (!b->retired && b->queued &&
        ((sequence && b->sequenced_only) ||
         (!sequence && !b->sequenced_only))) {
        size_t index = b->first;
        /* A legacy receiver cannot discard a sequenced message. A DATA
         * receiver cannot consume any other DATA ordinal, including a
         * legacy message. Denial leaves credits and destination unchanged. */
        if (b->mail_sequences[index] == sequence) {
            size_t n = b->lengths[index];
            *required_length = n;
            if (n > capacity || (n && !output)) {
                result = ORES_ACTOR_BYTES_SMALL;
            } else {
                unsigned char* source = b->slab + index * b->max_payload;
                if (n) memmove(output, source, n);
                /* Scrub the FULL physical slot on each successful dequeue. */
                scrub(source, b->max_payload);
                b->lengths[index] = 0;
                b->mail_sequences[index] = 0;
                b->first = index + 1 == b->capacity ? 0 : index + 1;
                b->queued--;
                b->used -= n;
                result = ORES_ACTOR_BYTES_OK;
            }
        }
    }
    release(b);
    return result;
}
int ores_native_actor_bytes_try_receive(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef receiver, void* output, size_t capacity,
    size_t* required_length) {
    return receive_internal(b, receiver, 0, output, capacity, required_length);
}
int ores_native_actor_bytes_try_receive_sequenced(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef receiver, uint64_t source_mail_sequence,
    void* output, size_t capacity, size_t* required_length) {
    if (!source_mail_sequence) return ORES_ACTOR_BYTES_DENIED;
    return receive_internal(b, receiver, source_mail_sequence,
                            output, capacity, required_length);
}

int ores_native_actor_bytes_try_retire(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef owner) {
    if (!b || !same(b, owner)) return ORES_ACTOR_BYTES_DENIED;
    if (!acquire(b)) return ORES_ACTOR_BYTES_BUSY;
    int result = ORES_ACTOR_BYTES_DENIED;
    if (!b->retired) {
        b->retired = 1;
        scrub(b->slab, b->capacity * b->max_payload);
        memset(b->lengths, 0, b->capacity * sizeof(size_t));
        scrub(b->mail_sequences, b->capacity * sizeof(uint64_t));
        b->queued = 0;
        b->used = 0;
        b->first = 0;
        b->next = 0;
        result = ORES_ACTOR_BYTES_OK;
    }
    release(b);
    return result;
}

int ores_native_actor_bytes_try_destroy(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef owner) {
    if (!b || !same(b, owner)) return ORES_ACTOR_BYTES_DENIED;
    if (!acquire(b)) return ORES_ACTOR_BYTES_BUSY;
    if (!b->retired || b->queued || b->used) {
        release(b);
        return ORES_ACTOR_BYTES_DENIED;
    }
    /* Runtime STOP/DONE must ensure there are no concurrent attempts to
     * enter after this point, not just an empty queue. */
    scrub(b->slab, b->capacity * b->max_payload);
    scrub(b->lengths, b->capacity * sizeof(size_t));
    scrub(b->mail_sequences, b->capacity * sizeof(uint64_t));
    scrub(&b->owner, sizeof(b->owner));
    free(b->mail_sequences);
    free(b->lengths);
    free(b->slab);
    free(b);
    return ORES_ACTOR_BYTES_OK;
}

int ores_native_actor_bytes_observe(OresNativeActorBytesInbox* b,
    OresNativeActorBytesRef owner, size_t* pending, size_t* used_bytes) {
    if (!b || !same(b, owner) || !pending || !used_bytes)
        return ORES_ACTOR_BYTES_DENIED;
    if (!acquire(b)) return ORES_ACTOR_BYTES_BUSY;
    *pending = b->queued;
    *used_bytes = b->used;
    release(b);
    return ORES_ACTOR_BYTES_OK;
}

#ifdef ORESLANG_ACTOR_BYTES_TESTING
int ores_native_actor_bytes_all_zero_for_test(OresNativeActorBytesInbox* b) {
    if (!b || !acquire(b)) return 0;
    int zero = 1;
    for (size_t i = 0; i < b->capacity * b->max_payload; ++i)
        if (b->slab[i]) { zero = 0; break; }
    release(b);
    return zero;
}
#endif
