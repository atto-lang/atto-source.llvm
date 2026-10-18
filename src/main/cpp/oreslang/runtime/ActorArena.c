#include "oreslang/runtime/ActorArena.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define ORES_MAX_SLOTS 4096u
#define ORES_MAX_LOANS 64u
#define ORES_MAX_SCRATCH_ALLOCS 4096u
#define ORES_MAX_SLOT_BYTES (1024u * 1024u)
#define ORES_MAX_SCRATCH_BYTES (16u * 1024u * 1024u)
/* Prevent parameter multiplication from creating a multi-GiB actor arena. */
#define ORES_MAX_ACTOR_BYTES (64u * 1024u * 1024u)
typedef struct { uint64_t generation; size_t length; uint32_t loans; unsigned char live; } Slot;
typedef struct { uint64_t nonce, index, generation; } LoanEntry;
typedef struct { uint64_t offset, length; } ScratchEntry;
struct OresActorArena {
    atomic_flag lock;
    uint64_t domain, owner, incarnation, epoch, next_loan_nonce;
    size_t slots, slot_bytes, scratch_bytes, scratch_top;
    uint32_t loans, scratch_count;
    int active, retiring;
    Slot* meta;
    /* All metadata is preallocated, so slot admission is O(1) and makes
     * no malloc/free calls during an actor turn. */
    size_t* free_slots;
    size_t free_count;
    unsigned char* slab;
    unsigned char* scratch;
    LoanEntry entries[ORES_MAX_LOANS];
    ScratchEntry scratch_entries[ORES_MAX_SCRATCH_ALLOCS];
};
static _Atomic(uint64_t) next_domain = 1;
/* A turn token alone is not execution authority. Bind the admitted turn to
 * its physical carrier; migration is allowed only after leave + new enter. */
static _Thread_local OresActorArena* carrier_arena;
static _Thread_local uint64_t carrier_epoch;
static void deny(void) { abort(); }
/* Security-sensitive arena bytes must be scrubbed even when the compiler can
 * prove that the backing allocation is freed or overwritten immediately after. */
static void secure_zero(void* ptr, size_t bytes) {
    volatile unsigned char* p = (volatile unsigned char*) ptr;
    while (bytes--) *p++ = 0;
}
/* Domain IDs are non-recyclable capability provenance. Permanently deny new
 * arenas at UINT64_MAX instead of wrapping through zero back to live IDs. */
static uint64_t claim_domain(void) {
    uint64_t candidate = atomic_load_explicit(&next_domain, memory_order_relaxed);
    for (;;) {
        if (!candidate || candidate == UINT64_MAX) deny();
        const uint64_t desired = candidate + 1;
        if (atomic_compare_exchange_weak_explicit(
                &next_domain, &candidate, desired,
                memory_order_relaxed, memory_order_relaxed))
            return candidate;
    }
}
static int try_enter_lock(OresActorArena* a) {
    if (!a) deny();
    return !atomic_flag_test_and_set_explicit(&a->lock, memory_order_acquire);
}
static void enter_lock(OresActorArena* a) {
    if (!a) deny();
    for (unsigned n = 0; n < 1000000u; ++n)
        if (try_enter_lock(a)) return;
    deny();
}
static void leave_lock(OresActorArena* a) {
    atomic_flag_clear_explicit(&a->lock, memory_order_release);
}
static void require_turn(OresActorArena* a, OresArenaTurn turn) {
    if (a->retiring || !a->active || turn.domain != a->domain
        || turn.epoch != a->epoch || carrier_arena != a
        || carrier_epoch != turn.epoch) deny();
}
static Slot* require_slot(OresActorArena* a, OresArenaHandle h) {
    if (h.domain != a->domain || h.index >= a->slots) deny();
    Slot* s = &a->meta[h.index];
    if (!s->live || s->generation != h.generation) deny();
    return s;
}
static void require_span(size_t len, size_t off, size_t n) {
    if (off > len || n > len - off) deny();
}
static size_t checked_mul(size_t a, size_t b) {
    if (a && b > SIZE_MAX / a) deny();
    return a * b;
}
OresActorArena* ores_actor_arena_open(uint64_t owner, uint64_t incarnation,
                                       size_t slots, size_t slot_bytes, size_t scratch_bytes) {
    if (!owner || !incarnation || !slots || slots > ORES_MAX_SLOTS
        || !slot_bytes || slot_bytes > ORES_MAX_SLOT_BYTES
        || !scratch_bytes || scratch_bytes > ORES_MAX_SCRATCH_BYTES) deny();
    const size_t slab_bytes = checked_mul(slots, slot_bytes);
    if (slab_bytes > ORES_MAX_ACTOR_BYTES
        || scratch_bytes > ORES_MAX_ACTOR_BYTES - slab_bytes) deny();
    (void) checked_mul(slots, sizeof(Slot));
    (void) checked_mul(slots, sizeof(size_t));
    OresActorArena* a = (OresActorArena*) calloc(1, sizeof(*a));
    if (!a) deny();
    a->meta = (Slot*) calloc(slots, sizeof(Slot));
    a->free_slots = (size_t*) calloc(slots, sizeof(size_t));
    a->slab = (unsigned char*) calloc(1, slab_bytes);
    a->scratch = (unsigned char*) calloc(1, scratch_bytes);
    if (!a->meta || !a->free_slots || !a->slab || !a->scratch) deny();
    atomic_flag_clear(&a->lock);
    a->domain = claim_domain();
    a->owner = owner;
    a->incarnation = incarnation;
    a->slots = slots;
    a->slot_bytes = slot_bytes;
    a->scratch_bytes = scratch_bytes;
    a->next_loan_nonce = 1;
    a->free_count = slots;
    for (size_t i = 0; i < slots; ++i) {
        a->meta[i].generation = 1;
        a->free_slots[i] = slots - 1 - i;
    }
    return a;
}
OresArenaTurn ores_actor_arena_enter(OresActorArena* a, uint64_t owner,
                                     uint64_t incarnation) {
    if (carrier_arena != NULL) deny();
    enter_lock(a);
    if (a->retiring || a->active || a->owner != owner
        || a->incarnation != incarnation || a->epoch == UINT64_MAX) deny();
    a->active = 1;
    ++a->epoch;
    OresArenaTurn turn = {a->domain, a->epoch};
    carrier_arena = a;
    carrier_epoch = turn.epoch;
    leave_lock(a);
    return turn;
}
void ores_actor_arena_leave(OresActorArena* a, OresArenaTurn turn) {
    enter_lock(a);
    /* Shutdown can mark retirement while an already admitted turn unwinds.
     * Leaving must remain legal after retirement, but all new reads/writes
     * are revoked by require_turn(). */
    if (!a->active || turn.domain != a->domain || turn.epoch != a->epoch
        || carrier_arena != a || carrier_epoch != turn.epoch) deny();
    secure_zero(a->scratch, a->scratch_top);
    if (a->scratch_count)
        secure_zero(a->scratch_entries,
                    (size_t) a->scratch_count * sizeof(a->scratch_entries[0]));
    a->scratch_top = 0;
    a->scratch_count = 0;
    a->active = 0;
    carrier_arena = NULL;
    carrier_epoch = 0;
    leave_lock(a);
}
OresArenaHandle ores_actor_arena_alloc(OresActorArena* a, OresArenaTurn turn,
                                       size_t bytes) {
    enter_lock(a);
    require_turn(a, turn);
    if (!bytes || bytes > a->slot_bytes) deny();
    if (!a->free_count) deny();
    const size_t i = a->free_slots[--a->free_count];
    if (i >= a->slots) deny();
    Slot* s = &a->meta[i];
    if (s->live || s->generation == UINT64_MAX) deny();
    s->live = 1;
    s->length = bytes;
    OresArenaHandle handle = {a->domain, (uint64_t) i, s->generation};
    leave_lock(a);
    return handle;
}
void ores_actor_arena_read(OresActorArena* a, OresArenaTurn turn,
                            OresArenaHandle handle, size_t offset, void* out, size_t bytes) {
    enter_lock(a);
    require_turn(a, turn);
    Slot* s = require_slot(a, handle);
    require_span(s->length, offset, bytes);
    if (bytes && !out) deny();
    if (bytes) memcpy(out, a->slab + (size_t) handle.index * a->slot_bytes + offset, bytes);
    leave_lock(a);
}
void ores_actor_arena_write(OresActorArena* a, OresArenaTurn turn,
                             OresArenaHandle handle, size_t offset, const void* src, size_t bytes) {
    enter_lock(a);
    require_turn(a, turn);
    Slot* s = require_slot(a, handle);
    require_span(s->length, offset, bytes);
    if (s->loans || (bytes && !src)) deny();
    if (bytes) memcpy(a->slab + (size_t) handle.index * a->slot_bytes + offset, src, bytes);
    leave_lock(a);
}
void ores_actor_arena_drop(OresActorArena* a, OresArenaTurn turn, OresArenaHandle h) {
    enter_lock(a);
    require_turn(a, turn);
    Slot* s = require_slot(a, h);
    if (s->loans || s->generation == UINT64_MAX) deny();
    secure_zero(a->slab + (size_t) h.index * a->slot_bytes, a->slot_bytes);
    s->length = 0;
    s->live = 0;
    ++s->generation;
    if (a->free_count >= a->slots) deny();
    a->free_slots[a->free_count++] = (size_t) h.index;
    leave_lock(a);
}
OresArenaLoan ores_actor_arena_borrow(OresActorArena* a, OresArenaTurn turn, OresArenaHandle h) {
    enter_lock(a);
    require_turn(a, turn);
    Slot* s = require_slot(a, h);
    if (s->loans == UINT32_MAX || a->loans >= ORES_MAX_LOANS
        || a->next_loan_nonce == UINT64_MAX) deny();
    size_t index = ORES_MAX_LOANS;
    for (size_t i = 0; i < ORES_MAX_LOANS; ++i) {
        if (!a->entries[i].nonce) { index = i; break; }
    }
    if (index == ORES_MAX_LOANS) deny();
    const uint64_t nonce = a->next_loan_nonce++;
    a->entries[index] = (LoanEntry) {nonce, h.index, h.generation};
    ++s->loans; ++a->loans;
    OresArenaLoan loan = {a->domain, nonce, h.index, h.generation};
    leave_lock(a);
    return loan;
}
void ores_actor_arena_release(OresActorArena* a, OresArenaLoan loan) {
    enter_lock(a);
    if (loan.domain != a->domain || !loan.nonce || !a->loans) deny();
    for (size_t i = 0; i < ORES_MAX_LOANS; ++i) {
        LoanEntry* e = &a->entries[i];
        if (e->nonce != loan.nonce) continue;
        if (e->index != loan.index || e->generation != loan.generation
            || loan.index >= a->slots) deny();
        Slot* s = &a->meta[loan.index];
        if (!s->live || s->generation != loan.generation || !s->loans) deny();
        --s->loans; --a->loans;
        memset(e, 0, sizeof(*e));
        leave_lock(a);
        return;
    }
    deny();
}
OresArenaScratch ores_actor_arena_scratch_alloc(OresActorArena* a, OresArenaTurn turn,
                                                  size_t bytes, size_t alignment) {
    enter_lock(a);
    require_turn(a, turn);
    if (!bytes || !alignment || alignment > 64 || (alignment & (alignment - 1))) deny();
    if (a->scratch_count >= ORES_MAX_SCRATCH_ALLOCS) deny();
    const size_t mask = alignment - 1;
    if (a->scratch_top > SIZE_MAX - mask) deny();
    const size_t at = (a->scratch_top + mask) & ~mask;
    if (at > a->scratch_bytes || bytes > a->scratch_bytes - at) deny();
    /* Old scratch is zeroized when each actor turn leaves. */
    a->scratch_top = at + bytes;
    a->scratch_entries[a->scratch_count++] = (ScratchEntry) {
        (uint64_t) at, (uint64_t) bytes
    };
    OresArenaScratch h = {a->domain, turn.epoch, (uint64_t) at, (uint64_t) bytes};
    leave_lock(a);
    return h;
}
static void scratch_bounds(OresActorArena* a, OresArenaScratch h,
                           size_t off, size_t bytes) {
    if (h.domain != a->domain || h.epoch != a->epoch
        || h.offset > a->scratch_top || h.length > a->scratch_top - (size_t) h.offset) deny();

    /* Bounds are not authority: require the exact start+length of one
     * allocation issued during this actor turn. Entries are monotonic by
     * offset, so binary search stays bounded even at the allocation cap. */
    size_t lo = 0, hi = a->scratch_count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (a->scratch_entries[mid].offset < h.offset) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= a->scratch_count
        || a->scratch_entries[lo].offset != h.offset
        || a->scratch_entries[lo].length != h.length) deny();
    require_span((size_t) h.length, off, bytes);
}
void ores_actor_arena_scratch_write(OresActorArena* a, OresArenaTurn turn,
                                    OresArenaScratch h, size_t off, const void* src, size_t bytes) {
    enter_lock(a);
    require_turn(a, turn); scratch_bounds(a, h, off, bytes);
    if (bytes && !src) deny();
    if (bytes) memcpy(a->scratch + (size_t) h.offset + off, src, bytes);
    leave_lock(a);
}
void ores_actor_arena_scratch_read(OresActorArena* a, OresArenaTurn turn,
                                   OresArenaScratch h, size_t off, void* out, size_t bytes) {
    enter_lock(a);
    require_turn(a, turn); scratch_bounds(a, h, off, bytes);
    if (bytes && !out) deny();
    if (bytes) memcpy(out, a->scratch + (size_t) h.offset + off, bytes);
    leave_lock(a);
}
void ores_actor_arena_retire(OresActorArena* a) {
    enter_lock(a); a->retiring = 1; leave_lock(a);
}
int ores_actor_arena_try_destroy(OresActorArena* a) {
    if (!a) deny();
    /* CONTROL try-destroy is observational: lock contention means the arena
     * is not quiescent yet, not a process-fatal scheduler failure. */
    if (!try_enter_lock(a)) return 0;
    if (!a->retiring) deny();
    if (a->active || a->loans) { leave_lock(a); return 0; }
    /* Trusted scheduler fences all timers/wakers/FFI holders before destroy.
     * Once quiescence is proven, never republish an unlocked object that is
     * about to be scrubbed/freed; the lock dies with the allocation. */
    secure_zero(a->slab, checked_mul(a->slots, a->slot_bytes));
    secure_zero(a->scratch, a->scratch_bytes);
    secure_zero(a->meta, checked_mul(a->slots, sizeof(Slot)));
    secure_zero(a->free_slots, checked_mul(a->slots, sizeof(size_t)));
    secure_zero(a->entries, sizeof(a->entries));
    secure_zero(a->scratch_entries, sizeof(a->scratch_entries));
    free(a->slab); free(a->scratch); free(a->meta); free(a->free_slots);
    secure_zero(a, sizeof(*a)); free(a);
    return 1;
}
