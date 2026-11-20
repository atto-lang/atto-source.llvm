#include "oreslang/runtime/ActorArena.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define ORES_MAX_SLOTS 4096u
#define ORES_MAX_LOANS 64u
#define ORES_MAX_SLOT_BYTES (1024u * 1024u)
#define ORES_MAX_SCRATCH_BYTES (16u * 1024u * 1024u)
/* Prevent parameter multiplication from creating a multi-GiB actor arena. */
#define ORES_MAX_ACTOR_BYTES (64u * 1024u * 1024u)
typedef struct { uint64_t generation, page_claim; size_t length, claim_count; uint32_t loans; unsigned char live; } Slot;
typedef struct { uint64_t nonce, index, generation; } LoanEntry;
struct OresActorArena {
    atomic_flag lock;
    uint64_t domain, owner, incarnation, epoch, next_loan_nonce, next_page_claim;
    size_t slots, slot_bytes, scratch_bytes, scratch_top;
    uint32_t loans;
    int active, retiring, prefaulted;
    Slot* meta;
    /* All metadata is preallocated, so slot admission is O(1) and makes
     * no malloc/free calls during an actor turn. */
    size_t* free_slots;
    size_t free_count;
    unsigned char* slab;
    unsigned char* scratch;
    LoanEntry entries[ORES_MAX_LOANS];
};
static _Atomic(uint64_t) next_domain = 1;
/* A turn token alone is not execution authority. Bind the admitted turn to
 * its physical carrier; migration is allowed only after leave + new enter. */
static _Thread_local OresActorArena* carrier_arena;
static _Thread_local uint64_t carrier_epoch;
static void deny(void) { abort(); }
/* Ordinary memset immediately before free can be eliminated at -O2.
 * Actor bytes and authority metadata must be scrubbed physically. */
static void scrub(void* memory, size_t bytes) {
    volatile unsigned char* p = (volatile unsigned char*) memory;
    for (size_t i = 0; i < bytes; ++i) p[i] = 0;
}
static void enter_lock(OresActorArena* a) {
    if (!a) deny();
    for (unsigned n = 0; n < 1000000u; ++n)
        if (!atomic_flag_test_and_set_explicit(&a->lock, memory_order_acquire)) return;
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
/* Never let external host copy buffers alias the actor's backing or
 * allocator authority. Overlap makes memcpy undefined and may corrupt
 * metadata or leak private state. Reject wrapping pointer spans too.
 * This check does not allocate, acquire an extra lock, or run on zero bytes. */
static int disjoint_host_span(const void* buffer, size_t bytes,
                              const void* private_buffer, size_t private_bytes) {
    const uintptr_t start = (uintptr_t) buffer;
    const uintptr_t owned = (uintptr_t) private_buffer;
    if (bytes > UINTPTR_MAX - start ||
        private_bytes > UINTPTR_MAX - owned) return 0;
    return start + bytes <= owned || owned + private_bytes <= start;
}
static void require_external_buffer(OresActorArena* a,
                                    const void* buffer, size_t bytes) {
    if (!bytes) return;
    if (!buffer ||
        !disjoint_host_span(buffer, bytes, a, sizeof(*a)) ||
        !disjoint_host_span(buffer, bytes, a->meta,
                            checked_mul(a->slots, sizeof(Slot))) ||
        !disjoint_host_span(buffer, bytes, a->free_slots,
                            checked_mul(a->slots, sizeof(size_t))) ||
        !disjoint_host_span(buffer, bytes, a->slab,
                            checked_mul(a->slots, a->slot_bytes)) ||
        !disjoint_host_span(buffer, bytes, a->scratch, a->scratch_bytes)) {
        deny();
    }
}
/* A volatile touch prevents the compiler from removing startup residency
 * preparation merely because calloc returned zero-filled storage. A 64-byte
 * stride is intentionally much smaller than supported OS page sizes. */
static void prefault_region(void* memory, size_t bytes) {
    if (!memory || !bytes) return;
    volatile unsigned char* p = (volatile unsigned char*) memory;
    for (size_t i = 0; i < bytes; i += 64u) {
        const unsigned char value = p[i];
        p[i] = value;
    }
    {
        const unsigned char value = p[bytes - 1u];
        p[bytes - 1u] = value;
    }
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
    a->domain = atomic_fetch_add_explicit(&next_domain, 1, memory_order_relaxed);
    if (!a->domain || a->domain == UINT64_MAX) deny();
    a->owner = owner;
    a->incarnation = incarnation;
    a->slots = slots;
    a->slot_bytes = slot_bytes;
    a->scratch_bytes = scratch_bytes;
    a->next_loan_nonce = 1;
    a->next_page_claim = 1;
    a->free_count = slots;
    for (size_t i = 0; i < slots; ++i) {
        a->meta[i].generation = 1;
        a->free_slots[i] = slots - 1 - i;
    }
    return a;
}
int ores_actor_arena_prefault(OresActorArena* a) {
    enter_lock(a);
    if (a->active || a->retiring) {
        leave_lock(a);
        return 0;
    }
    prefault_region(a->meta, checked_mul(a->slots, sizeof(Slot)));
    prefault_region(a->free_slots, checked_mul(a->slots, sizeof(size_t)));
    prefault_region(a->slab, checked_mul(a->slots, a->slot_bytes));
    prefault_region(a->scratch, a->scratch_bytes);
    prefault_region(a->entries, sizeof(a->entries));
    a->prefaulted = 1;
    leave_lock(a);
    return 1;
}
int ores_actor_arena_is_prefaulted(OresActorArena* a) {
    int prefaulted;
    enter_lock(a);
    prefaulted = a->prefaulted;
    leave_lock(a);
    return prefaulted;
}
static OresArenaTurn enter_impl(
        OresActorArena* a, uint64_t owner, uint64_t incarnation,
        int require_prefault) {
    if (carrier_arena != NULL) deny();
    enter_lock(a);
    if (a->retiring || a->active || a->owner != owner
        || a->incarnation != incarnation || a->epoch == UINT64_MAX
        || (require_prefault && !a->prefaulted)) deny();
    a->active = 1;
    ++a->epoch;
    OresArenaTurn turn = {a->domain, a->epoch};
    carrier_arena = a;
    carrier_epoch = turn.epoch;
    leave_lock(a);
    return turn;
}
OresArenaTurn ores_actor_arena_enter(OresActorArena* a, uint64_t owner,
                                     uint64_t incarnation) {
    return enter_impl(a, owner, incarnation, 0);
}
OresArenaTurn ores_actor_arena_enter_realtime(
        OresActorArena* a, uint64_t owner, uint64_t incarnation) {
    return enter_impl(a, owner, incarnation, 1);
}
void ores_actor_arena_leave(OresActorArena* a, OresArenaTurn turn) {
    enter_lock(a);
    /* Shutdown can mark retirement while an already admitted turn unwinds.
     * Leaving must remain legal after retirement, but all new reads/writes
     * are revoked by require_turn(). */
    if (!a->active || turn.domain != a->domain || turn.epoch != a->epoch
        || carrier_arena != a || carrier_epoch != turn.epoch) deny();
    scrub(a->scratch, a->scratch_top);
    a->scratch_top = 0;
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
    require_external_buffer(a, out, bytes);
    if (bytes) memcpy(out, a->slab + (size_t) handle.index * a->slot_bytes + offset, bytes);
    leave_lock(a);
}
void ores_actor_arena_write(OresActorArena* a, OresArenaTurn turn,
                             OresArenaHandle handle, size_t offset, const void* src, size_t bytes) {
    enter_lock(a);
    require_turn(a, turn);
    Slot* s = require_slot(a, handle);
    require_span(s->length, offset, bytes);
    if (s->loans) deny();
    require_external_buffer(a, src, bytes);
    if (bytes) memcpy(a->slab + (size_t) handle.index * a->slot_bytes + offset, src, bytes);
    leave_lock(a);
}
int ores_actor_arena_validate_pages(OresActorArena* a, OresArenaTurn turn,
                                    const OresArenaHandle* pages,
                                    size_t count, size_t page_bytes) {
    enter_lock(a);
    require_turn(a, turn);
    if (!pages || !count || count > a->slots || !page_bytes
            || page_bytes > a->slot_bytes) {
        leave_lock(a);
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        const OresArenaHandle h = pages[i];
        if (h.domain != a->domain || h.index >= a->slots) {
            leave_lock(a);
            return 0;
        }
        const Slot* slot = &a->meta[h.index];
        if (!slot->live || slot->generation != h.generation
                || slot->length != page_bytes) {
            leave_lock(a);
            return 0;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pages[j].index == h.index) {
                leave_lock(a);
                return 0;
            }
        }
    }
    leave_lock(a);
    return 1;
}

/* One lock protects validation and all reservations. No caller sees
 * partially claimed multi-page storage, including on late-slot rejection. */
uint64_t ores_actor_arena_claim_pages(OresActorArena* a, OresArenaTurn turn,
                                      const OresArenaHandle* pages,
                                      size_t count, size_t page_bytes) {
    enter_lock(a);
    require_turn(a, turn);
    if (!pages || !count || count > a->slots || !page_bytes
            || page_bytes > a->slot_bytes || !a->next_page_claim
            || a->next_page_claim == UINT64_MAX) {
        leave_lock(a);
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        const OresArenaHandle h = pages[i];
        if (h.domain != a->domain || h.index >= a->slots) {
            leave_lock(a);
            return 0;
        }
        const Slot* s = &a->meta[h.index];
        if (!s->live || s->generation != h.generation
                || s->length != page_bytes || s->page_claim || s->loans) {
            leave_lock(a);
            return 0;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pages[j].index == h.index) {
                leave_lock(a);
                return 0;
            }
        }
    }
    const uint64_t claim = a->next_page_claim++;
    for (size_t i = 0; i < count; ++i) {
        Slot* s = &a->meta[pages[i].index];
        s->page_claim = claim;
        s->claim_count = count;
    }
    leave_lock(a);
    return claim;
}

int ores_actor_arena_unclaim_pages(OresActorArena* a, OresArenaTurn turn,
                                   const OresArenaHandle* pages,
                                   size_t count, uint64_t claim) {
    enter_lock(a);
    require_turn(a, turn);
    if (!claim || !pages || !count || count > a->slots) {
        leave_lock(a);
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        const OresArenaHandle h = pages[i];
        if (h.domain != a->domain || h.index >= a->slots) {
            leave_lock(a);
            return 0;
        }
        const Slot* s = &a->meta[h.index];
        if (!s->live || s->generation != h.generation
                || s->page_claim != claim || s->claim_count != count
                || s->loans) {
            leave_lock(a);
            return 0;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pages[j].index == h.index) {
                leave_lock(a);
                return 0;
            }
        }
    }
    for (size_t i = 0; i < count; ++i) {
        Slot* s = &a->meta[pages[i].index];
        s->page_claim = 0;
        s->claim_count = 0;
    }
    leave_lock(a);
    return 1;
}

void ores_actor_arena_drop(OresActorArena* a, OresArenaTurn turn, OresArenaHandle h) {
    enter_lock(a);
    require_turn(a, turn);
    Slot* s = require_slot(a, h);
    if (s->loans || s->page_claim || s->generation == UINT64_MAX) deny();
    scrub(a->slab + (size_t) h.index * a->slot_bytes, a->slot_bytes);
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
    if (s->page_claim || s->loans == UINT32_MAX || a->loans >= ORES_MAX_LOANS
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
    const size_t mask = alignment - 1;
    if (a->scratch_top > SIZE_MAX - mask) deny();
    const size_t at = (a->scratch_top + mask) & ~mask;
    if (at > a->scratch_bytes || bytes > a->scratch_bytes - at) deny();
    /* Old scratch is zeroized when each actor turn leaves. */
    a->scratch_top = at + bytes;
    OresArenaScratch h = {a->domain, turn.epoch, (uint64_t) at, (uint64_t) bytes};
    leave_lock(a);
    return h;
}
static void scratch_bounds(OresActorArena* a, OresArenaScratch h,
                           size_t off, size_t bytes) {
    if (h.domain != a->domain || h.epoch != a->epoch
        || h.offset > a->scratch_top || h.length > a->scratch_top - (size_t) h.offset) deny();
    require_span((size_t) h.length, off, bytes);
}
void ores_actor_arena_scratch_write(OresActorArena* a, OresArenaTurn turn,
                                    OresArenaScratch h, size_t off, const void* src, size_t bytes) {
    enter_lock(a);
    require_turn(a, turn); scratch_bounds(a, h, off, bytes);
    require_external_buffer(a, src, bytes);
    if (bytes) memcpy(a->scratch + (size_t) h.offset + off, src, bytes);
    leave_lock(a);
}
void ores_actor_arena_scratch_read(OresActorArena* a, OresArenaTurn turn,
                                   OresArenaScratch h, size_t off, void* out, size_t bytes) {
    enter_lock(a);
    require_turn(a, turn); scratch_bounds(a, h, off, bytes);
    require_external_buffer(a, out, bytes);
    if (bytes) memcpy(out, a->scratch + (size_t) h.offset + off, bytes);
    leave_lock(a);
}
void ores_actor_arena_retire(OresActorArena* a) {
    enter_lock(a); a->retiring = 1; leave_lock(a);
}
int ores_actor_arena_try_destroy(OresActorArena* a) {
    enter_lock(a);
    if (!a->retiring) deny();
    if (a->active || a->loans) { leave_lock(a); return 0; }
    for (size_t i = 0; i < a->slots; ++i) {
        if (a->meta[i].page_claim) { leave_lock(a); return 0; }
    }
    /* Trusted scheduler fences all timers/wakers/FFI holders before destroy. */
    scrub(a->slab, checked_mul(a->slots, a->slot_bytes));
    scrub(a->scratch, a->scratch_bytes);
    scrub(a->meta, checked_mul(a->slots, sizeof(Slot)));
    scrub(a->free_slots, checked_mul(a->slots, sizeof(size_t)));
    free(a->slab); free(a->scratch); free(a->meta); free(a->free_slots);
    leave_lock(a);
    scrub(a, sizeof(*a)); free(a);
    return 1;
}

