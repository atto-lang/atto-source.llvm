#include "ControlPlaneArena32.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_BITS 20u
#define SLOT_MASK ((1u << SLOT_BITS) - 1u)
#define MAX_GENERATION ((1u << (32u - SLOT_BITS)) - 1u)

typedef struct {
    uint32_t generation;
    uint32_t live;
    uint32_t free_next;
} ControlSlot;
struct AttoControlPlaneArena32 {
    pthread_mutex_t mutex;
    uint8_t *records;
    ControlSlot *slots;
    uint32_t capacity;
    uint32_t issuer;
    uint32_t next_slot;
    uint32_t free_head;
    int closed;
};
static atomic_uint_fast64_t next_issuer = ATOMIC_VAR_INIT(0);

static uint64_t witness(const AttoControlPlaneArena32 *a, uint32_t word) {
    return ((uint64_t)a->issuer << 32) | word;
}
static int validate_locked(AttoControlPlaneArena32 *a, uint64_t ref, uint32_t *slot) {
    if (a->closed || (uint32_t)(ref >> 32) != a->issuer) return 0;
    uint32_t word = (uint32_t)ref;
    uint32_t low = word & SLOT_MASK, gen = word >> SLOT_BITS;
    if (!low || low > a->capacity || !gen) return 0;
    uint32_t i = low - 1u;
    if (!a->slots[i].live || a->slots[i].generation != gen) return 0;
    *slot = i;
    return 1;
}
static size_t record_offset(uint32_t slot, size_t field) {
    return (size_t)slot * ATTO_CONTROL_RECORD_BYTES + field;
}
static void put_u64_le(uint8_t *dst, uint64_t word) {
    for (unsigned i = 0; i < 8; ++i) dst[i] = (uint8_t)(word >> (8u * i));
}
static uint64_t get_u64_le(const uint8_t *src) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; ++i) result |= (uint64_t)src[i] << (8u * i);
    return result;
}
static void put_u32_le(uint8_t *dst, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = (uint8_t)(word >> (8u * i));
}
static uint32_t get_u32_le(const uint8_t *src) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result |= (uint32_t)src[i] << (8u * i);
    return result;
}
AttoControlPlaneArena32 *atto_control32_new(uint32_t capacity) {
    if (!capacity || capacity > SLOT_MASK) return NULL;
    uint_fast64_t id = atomic_fetch_add_explicit(&next_issuer, 1, memory_order_relaxed) + 1;
    if (id > UINT32_MAX || id == 0) return NULL;
    AttoControlPlaneArena32 *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->issuer = (uint32_t)id;
    a->capacity = capacity;
    if (pthread_mutex_init(&a->mutex, NULL)) { free(a); return NULL; }
    a->records = calloc((size_t)capacity, ATTO_CONTROL_RECORD_BYTES);
    a->slots = calloc((size_t)capacity, sizeof(*a->slots));
    if (!a->records || !a->slots) {
        free(a->records); free(a->slots);
        pthread_mutex_destroy(&a->mutex); free(a); return NULL;
    }
    for (uint32_t i = 0; i < capacity; ++i) a->slots[i].generation = 1;
    return a;
}
int atto_control32_allocate(AttoControlPlaneArena32 *a, uint64_t *out) {
    if (out) *out = 0;
    if (!a || !out) return 0;
    pthread_mutex_lock(&a->mutex);
    uint32_t i;
    if (a->closed) { pthread_mutex_unlock(&a->mutex); return 0; }
    if (a->free_head) {
        i = a->free_head - 1u;
        a->free_head = a->slots[i].free_next;
    } else {
        if (a->next_slot >= a->capacity) { pthread_mutex_unlock(&a->mutex); return 0; }
        i = a->next_slot++;
    }
    if (!a->slots[i].generation || a->slots[i].live) {
        pthread_mutex_unlock(&a->mutex); return 0;
    }
    a->slots[i].live = 1;
    a->slots[i].free_next = 0;
    *out = witness(a, (a->slots[i].generation << SLOT_BITS) | (i + 1u));
    pthread_mutex_unlock(&a->mutex);
    return 1;
}
int atto_control32_drop(AttoControlPlaneArena32 *a, uint64_t ref) {
    if (!a) return 0;
    pthread_mutex_lock(&a->mutex);
    uint32_t i;
    if (!validate_locked(a, ref, &i)) { pthread_mutex_unlock(&a->mutex); return 0; }
    memset(a->records + record_offset(i, 0), 0, ATTO_CONTROL_RECORD_BYTES);
    a->slots[i].live = 0;
    if (a->slots[i].generation == MAX_GENERATION) a->slots[i].generation = 0;
    else {
        ++a->slots[i].generation;
        a->slots[i].free_next = a->free_head;
        a->free_head = i + 1u;
    }
    pthread_mutex_unlock(&a->mutex);
    return 1;
}
int atto_control32_write_i64(AttoControlPlaneArena32 *a, uint64_t ref, unsigned field, int64_t value) {
    if (!a) return 0;
    pthread_mutex_lock(&a->mutex);
    uint32_t i;
    int ok = (field == 0u || field == 8u) && validate_locked(a, ref, &i);
    if (ok) put_u64_le(a->records + record_offset(i, field), (uint64_t)value);
    pthread_mutex_unlock(&a->mutex);
    return ok;
}
int atto_control32_read_i64(AttoControlPlaneArena32 *a, uint64_t ref, unsigned field, int64_t *out) {
    if (out) *out = 0;
    if (!a || !out) return 0;
    pthread_mutex_lock(&a->mutex);
    uint32_t i;
    int ok = (field == 0u || field == 8u) && validate_locked(a, ref, &i);
    if (ok) {
        uint64_t bits = get_u64_le(a->records + record_offset(i, field));
        memcpy(out, &bits, sizeof(bits));
    }
    pthread_mutex_unlock(&a->mutex);
    return ok;
}
int atto_control32_write_ref(AttoControlPlaneArena32 *a, uint64_t owner, uint64_t child) {
    if (!a) return 0;
    pthread_mutex_lock(&a->mutex);
    uint32_t i, checked_child;
    int ok = validate_locked(a, owner, &i) && (!child || validate_locked(a, child, &checked_child));
    if (ok) put_u32_le(a->records + record_offset(i, 16), (uint32_t)child);
    pthread_mutex_unlock(&a->mutex);
    return ok;
}
int atto_control32_read_ref(AttoControlPlaneArena32 *a, uint64_t owner, uint64_t *out) {
    if (out) *out = 0;
    if (!a || !out) return 0;
    pthread_mutex_lock(&a->mutex);
    uint32_t i, child_slot;
    int ok = validate_locked(a, owner, &i);
    if (ok) {
        uint32_t word = get_u32_le(a->records + record_offset(i, 16));
        uint64_t child = word ? witness(a, word) : 0;
        ok = !word || validate_locked(a, child, &child_slot);
        if (ok) *out = child;
    }
    pthread_mutex_unlock(&a->mutex);
    return ok;
}
void atto_control32_retire(AttoControlPlaneArena32 *a) {
    if (!a) return;
    pthread_mutex_lock(&a->mutex);
    if (!a->closed) {
        a->closed = 1;
        memset(a->records, 0, (size_t)a->capacity * ATTO_CONTROL_RECORD_BYTES);
        memset(a->slots, 0, (size_t)a->capacity * sizeof(*a->slots));
    }
    pthread_mutex_unlock(&a->mutex);
}
void atto_control32_destroy(AttoControlPlaneArena32 *a) {
    if (!a) return;
    /* Caller must prove no other carrier, callback, FFI pin or loan is active. */
    atto_control32_retire(a);
    pthread_mutex_destroy(&a->mutex);
    free(a->records); free(a->slots); free(a);
}
