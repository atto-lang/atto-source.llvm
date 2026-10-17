#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Fixed per-actor slot and loan caps; zero guest tracing/RC.
 * This is the serial-actor native substrate. Concurrent actor messages need
 * a compiler/scheduler admission barrier before they may use this backend. */
#define MAX_SLOTS 4096u
#define MAX_LOANS 64u
#define MAX_EXTERNAL_LEASES 64u

typedef struct {
    int64_t value;
    uint64_t generation;
    uint32_t loans;
    unsigned char live;
    unsigned char field_mutable;
    unsigned char fixed_field;
} ActorSlot;

typedef struct {
    uint64_t nonce;
    uint64_t index;
    uint64_t generation;
} ActorLoanEntry;

typedef struct {
    uint64_t generation;
    unsigned char active;
} ActorExternalLeaseEntry;

struct OresActorI64Heap {
    atomic_flag mutex;
    uint64_t domain;
    uint64_t owner;
    uint64_t incarnation;
    uint64_t capacity;
    uint64_t turn_epoch;
    uint64_t next_loan_nonce;
    int turn_active;
    int retiring;
    uint32_t active_loans;
    uint32_t active_external_leases;
    ActorLoanEntry loan_entries[MAX_LOANS];
    ActorExternalLeaseEntry external_leases[MAX_EXTERNAL_LEASES];
    ActorSlot slots[];
};

static _Atomic(uint64_t) next_domain = 1;
/* A logical turn token is not physical execution authority. The carrier that
 * admitted the turn is the only carrier allowed to dereference actor memory;
 * migration requires leave + a fresh enter on the destination carrier. */
static _Thread_local OresActorI64Heap* carrier_heap;
static _Thread_local uint64_t carrier_epoch;

static void rejected(void) { abort(); }

/* Domain authority is never recycled. A wrapping fetch_add would transiently
 * publish zero and then restart at one; use a saturating CAS loop instead so
 * exhaustion is permanently fail-closed and stale handles can never regain a
 * previously issued domain. */
static uint64_t allocate_domain(void) {
    uint64_t current = atomic_load_explicit(&next_domain, memory_order_relaxed);
    for (;;) {
        if (current == 0 || current == UINT64_MAX) rejected();
        const uint64_t next = current + 1;
        if (atomic_compare_exchange_weak_explicit(
                &next_domain, &current, next,
                memory_order_relaxed, memory_order_relaxed)) {
            return current;
        }
    }
}

/* Short critical sections; bounded spinning fails rather than blocking an
 * uncooperative carrier forever. Production actor scheduling must not allow
 * guest code to execute while this lock is held. */
static int try_lock_heap(OresActorI64Heap* heap) {
    if (!heap) rejected();
    return !atomic_flag_test_and_set_explicit(&heap->mutex, memory_order_acquire);
}
static void lock_heap(OresActorI64Heap* heap) {
    if (!heap) rejected();
    for (unsigned i = 0; i != 1000000u; ++i) {
        if (try_lock_heap(heap)) return;
    }
    rejected();
}
static void unlock_heap(OresActorI64Heap* heap) {
    atomic_flag_clear_explicit(&heap->mutex, memory_order_release);
}

static void require_turn(OresActorI64Heap* heap, OresActorTurn turn) {
    if (heap->retiring || !heap->turn_active
        || heap->domain != turn.domain || heap->turn_epoch != turn.epoch
        || carrier_heap != heap || carrier_epoch != turn.epoch) {
        rejected();
    }
}
static ActorSlot* checked_slot(OresActorI64Heap* heap, OresActorSlot slot) {
    if (slot.domain != heap->domain || slot.index >= heap->capacity) rejected();
    ActorSlot* target = &heap->slots[slot.index];
    if (!target->live || target->generation != slot.generation) rejected();
    return target;
}

OresActorI64Heap* ores_actor_i64_heap_open(uint64_t actor_id,
                                            uint64_t incarnation,
                                            uint64_t slot_capacity) {
    if (!actor_id || !incarnation || !slot_capacity || slot_capacity > MAX_SLOTS) rejected();
    if (slot_capacity > (SIZE_MAX - offsetof(OresActorI64Heap, slots)) / sizeof(ActorSlot))
        rejected();
    const size_t bytes = offsetof(OresActorI64Heap, slots)
                       + (size_t) slot_capacity * sizeof(ActorSlot);
    OresActorI64Heap* heap = (OresActorI64Heap*) calloc(1, bytes);
    if (!heap) rejected();
    atomic_flag_clear(&heap->mutex);
    heap->domain = allocate_domain();
    heap->owner = actor_id;
    heap->incarnation = incarnation;
    heap->capacity = slot_capacity;
    heap->next_loan_nonce = 1;
    for (uint64_t i = 0; i < slot_capacity; ++i) heap->slots[i].generation = 1;
    return heap;
}

OresActorTurn ores_actor_i64_heap_enter(OresActorI64Heap* heap,
                                          uint64_t actor_id,
                                          uint64_t incarnation) {
    if (carrier_heap != NULL) rejected();
    lock_heap(heap);
    if (heap->retiring || heap->turn_active
        || actor_id != heap->owner || incarnation != heap->incarnation
        || heap->turn_epoch == UINT64_MAX) rejected();
    heap->turn_active = 1;
    ++heap->turn_epoch;
    OresActorTurn token = {heap->domain, heap->turn_epoch};
    carrier_heap = heap;
    carrier_epoch = token.epoch;
    unlock_heap(heap);
    return token;
}

int ores_actor_i64_heap_try_enter(OresActorI64Heap* heap,
                                  uint64_t actor_id,
                                  uint64_t incarnation,
                                  OresActorTurn* out_turn) {
    if (!out_turn || !heap) rejected();
    // A denied retry never leaves a previously filled caller token usable.
    out_turn->domain = 0;
    out_turn->epoch = 0;
    if (carrier_heap != NULL) rejected();
    if (!try_lock_heap(heap)) return 0;
    if (heap->retiring || !actor_id || !incarnation
        || actor_id != heap->owner || incarnation != heap->incarnation
        || heap->turn_epoch == UINT64_MAX) rejected();
    if (heap->turn_active) {
        unlock_heap(heap);
        return 0;
    }
    heap->turn_active = 1;
    ++heap->turn_epoch;
    out_turn->domain = heap->domain;
    out_turn->epoch = heap->turn_epoch;
    carrier_heap = heap;
    carrier_epoch = out_turn->epoch;
    unlock_heap(heap);
    return 1;
}

void ores_actor_i64_heap_leave(OresActorI64Heap* heap, OresActorTurn turn) {
    lock_heap(heap);
    if (!heap->turn_active || turn.domain != heap->domain
            || turn.epoch != heap->turn_epoch || carrier_heap != heap
            || carrier_epoch != turn.epoch) rejected();
    heap->turn_active = 0;
    carrier_heap = NULL;
    carrier_epoch = 0;
    unlock_heap(heap);
}
OresActorSlot ores_actor_i64_heap_new(OresActorI64Heap* heap,
                                        OresActorTurn turn, int64_t value) {
    lock_heap(heap);
    require_turn(heap, turn);
    for (uint64_t i = 0; i < heap->capacity; ++i) {
        ActorSlot* s = &heap->slots[i];
        if (!s->live && s->generation != UINT64_MAX) {
            s->value = value;
            s->field_mutable = 0;
            s->fixed_field = 0;
            s->live = 1;
            OresActorSlot id = {heap->domain, i, s->generation};
            unlock_heap(heap);
            return id;
        }
    }
    rejected();
    OresActorSlot unreachable = {0, 0, 0};
    return unreachable;
}
int64_t ores_actor_i64_heap_read(OresActorI64Heap* heap,
                                   OresActorTurn turn, OresActorSlot slot) {
    lock_heap(heap);
    require_turn(heap, turn);
    const int64_t value = checked_slot(heap, slot)->value;
    unlock_heap(heap);
    return value;
}
void ores_actor_i64_heap_write(OresActorI64Heap* heap,
                                  OresActorTurn turn,
                                  OresActorSlot slot, int64_t value) {
    lock_heap(heap);
    require_turn(heap, turn);
    ActorSlot* target = checked_slot(heap, slot);
    /* A suspended shared read loan still freezes the referent: a new actor
     * turn is not permission to mutate it. Mutable loans need a separate
     * exclusive API and source-level alias proof. */
    if (target->fixed_field || target->loans != 0) rejected();
    target->value = value;
    unlock_heap(heap);
}
void ores_actor_i64_heap_drop(OresActorI64Heap* heap,
                                 OresActorTurn turn, OresActorSlot slot) {
    lock_heap(heap);
    require_turn(heap, turn);
    ActorSlot* s = checked_slot(heap, slot);
    if (s->fixed_field || s->loans != 0 || s->generation == UINT64_MAX) rejected();
    s->value = 0;
    s->live = 0;
    ++s->generation;
    unlock_heap(heap);
}

OresActorLoan ores_actor_i64_heap_borrow(OresActorI64Heap* heap,
                                           OresActorTurn turn, OresActorSlot slot) {
    lock_heap(heap);
    require_turn(heap, turn);
    ActorSlot* s = checked_slot(heap, slot);
    if (s->loans == UINT32_MAX || heap->active_loans >= MAX_LOANS
        || heap->next_loan_nonce == UINT64_MAX) rejected();
    unsigned found = MAX_LOANS;
    for (unsigned i = 0; i < MAX_LOANS; ++i) {
        if (heap->loan_entries[i].nonce == 0) { found = i; break; }
    }
    if (found == MAX_LOANS) rejected();
    uint64_t nonce = heap->next_loan_nonce++;
    heap->loan_entries[found].nonce = nonce;
    heap->loan_entries[found].index = slot.index;
    heap->loan_entries[found].generation = slot.generation;
    ++heap->active_loans;
    ++s->loans;
    OresActorLoan result = {heap->domain, nonce, slot.index, slot.generation};
    unlock_heap(heap);
    return result;
}
void ores_actor_i64_heap_release(OresActorI64Heap* heap, OresActorLoan loan) {
    lock_heap(heap);
    if (loan.domain != heap->domain || loan.nonce == 0 || !heap->active_loans)
        rejected();
    for (unsigned i = 0; i < MAX_LOANS; ++i) {
        ActorLoanEntry* entry = &heap->loan_entries[i];
        if (entry->nonce != loan.nonce) continue;
        if (entry->index != loan.index || entry->generation != loan.generation
            || loan.index >= heap->capacity) rejected();
        ActorSlot* s = &heap->slots[loan.index];
        if (!s->live || s->generation != loan.generation || !s->loans)
            rejected();
        --s->loans;
        --heap->active_loans;
        memset(entry, 0, sizeof(*entry));
        unlock_heap(heap);
        return;
    }
    rejected();
}

OresActorExternalLease ores_actor_i64_heap_external_acquire(
    OresActorI64Heap* heap, OresActorTurn turn) {
    lock_heap(heap);
    require_turn(heap, turn);
    if (heap->active_external_leases >= MAX_EXTERNAL_LEASES) rejected();

    unsigned found = MAX_EXTERNAL_LEASES;
    for (unsigned i = 0; i < MAX_EXTERNAL_LEASES; ++i) {
        ActorExternalLeaseEntry* candidate = &heap->external_leases[i];
        /*
         * Generation exhaustion permanently retires only this bounded lease
         * slot. Do not recycle its authority, but also do not let one retired
         * slot deny admission while another slot remains safe to issue.
         */
        if (!candidate->active && candidate->generation != UINT64_MAX) {
            found = i;
            break;
        }
    }
    if (found == MAX_EXTERNAL_LEASES) rejected();

    ActorExternalLeaseEntry* entry = &heap->external_leases[found];
    ++entry->generation;
    entry->active = 1;
    ++heap->active_external_leases;

    OresActorExternalLease lease = {
        heap->domain, (uint64_t) found, entry->generation
    };
    unlock_heap(heap);
    return lease;
}

void ores_actor_i64_heap_external_release(
    OresActorI64Heap* heap, OresActorExternalLease* lease) {
    if (!lease) rejected();

    /* Same-token cleanup retry after successful release must not even inspect
     * heap: CONTROL may already have reclaimed it. The scheduler must serialize
     * ownership of this mutable token; copied tokens are stale-replay evidence,
     * not independent lifetime owners. */
    if (lease->domain == 0 && lease->index == 0 && lease->generation == 0) return;

    lock_heap(heap);
    if (lease->domain != heap->domain || lease->index >= MAX_EXTERNAL_LEASES
        || lease->generation == 0) {
        rejected();
    }

    ActorExternalLeaseEntry* entry = &heap->external_leases[lease->index];
    if (lease->generation > entry->generation) rejected();

    /* An older generation cannot affect a newer lease occupying this bounded
     * slot. Consume the stale token so further cleanup retries become no-ops. */
    if (lease->generation < entry->generation || !entry->active) {
        memset(lease, 0, sizeof(*lease));
        unlock_heap(heap);
        return;
    }
    if (!heap->active_external_leases) rejected();
    entry->active = 0;
    --heap->active_external_leases;
    memset(lease, 0, sizeof(*lease));
    unlock_heap(heap);
}
void ores_actor_i64_heap_field_init(OresActorI64Heap* heap,
                                    uint64_t domain, uint64_t epoch,
                                    uint64_t index, int64_t initial) {
    lock_heap(heap);
    OresActorTurn turn = {domain, epoch};
    require_turn(heap, turn);
    if (index >= heap->capacity) rejected();
    ActorSlot* field = &heap->slots[index];
    // Unlike an ordinary slot, source-level actor fields are initialized
    // exactly once during the actor's first admitted turn.
    if (field->live || field->generation != 1) rejected();
    field->value = initial;
    field->fixed_field = 1;
    field->live = 1;
    unlock_heap(heap);
}

void ores_actor_i64_heap_field_init_mutable(OresActorI64Heap* heap,
                                           uint64_t domain, uint64_t epoch,
                                           uint64_t index, int64_t initial) {
    lock_heap(heap);
    OresActorTurn turn = {domain, epoch};
    require_turn(heap, turn);
    if (index >= heap->capacity) rejected();
    ActorSlot* field = &heap->slots[index];
    if (field->live || field->generation != 1) rejected();
    field->value = initial;
    field->field_mutable = 1;
    field->fixed_field = 1;
    field->live = 1;
    unlock_heap(heap);
}

void ores_actor_i64_heap_field_write(OresActorI64Heap* heap,
                                     uint64_t domain, uint64_t epoch,
                                     uint64_t index, int64_t value) {
    lock_heap(heap);
    OresActorTurn turn = {domain, epoch};
    require_turn(heap, turn);
    if (index >= heap->capacity) rejected();
    ActorSlot* field = &heap->slots[index];
    if (!field->live || !field->fixed_field || !field->field_mutable || field->loans != 0) rejected();
    field->value = value;
    unlock_heap(heap);
}

int64_t ores_actor_i64_heap_field_read(OresActorI64Heap* heap,
                                       uint64_t domain, uint64_t epoch,
                                       uint64_t index) {
    lock_heap(heap);
    OresActorTurn turn = {domain, epoch};
    require_turn(heap, turn);
    if (index >= heap->capacity || !heap->slots[index].live
        || !heap->slots[index].fixed_field) rejected();
    const int64_t value = heap->slots[index].value;
    unlock_heap(heap);
    return value;
}

void ores_actor_i64_heap_retire(OresActorI64Heap* heap) {
    lock_heap(heap);
    heap->retiring = 1; /* no subsequent actor turn may enter */
    unlock_heap(heap);
}
int ores_actor_i64_heap_try_destroy(OresActorI64Heap* heap) {
    if (!heap) rejected();
    /* CONTROL try-destroy must not spin behind an in-flight actor operation. */
    if (!try_lock_heap(heap)) return 0;
    if (!heap->retiring) rejected();
    if (heap->turn_active || heap->active_loans || heap->active_external_leases) {
        unlock_heap(heap);
        return 0;
    }
    const size_t bytes = offsetof(OresActorI64Heap, slots)
                       + (size_t) heap->capacity * sizeof(ActorSlot);
    memset(heap->slots, 0, (size_t) heap->capacity * sizeof(ActorSlot));
    memset(heap->loan_entries, 0, sizeof(heap->loan_entries));
    memset(heap->external_leases, 0, sizeof(heap->external_leases));
    heap->domain = 0;
    /* Release lock before teardown; the supervisor must have fenced all
     * transport, scheduler, timer and callback handles to this heap. */
    unlock_heap(heap);
    memset(heap, 0, bytes);
    free(heap);
    return 1;
}
