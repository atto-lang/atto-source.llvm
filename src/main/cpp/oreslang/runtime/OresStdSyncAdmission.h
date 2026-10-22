#ifndef ORESLANG_STD_SYNC_ADMISSION_H
#define ORESLANG_STD_SYNC_ADMISSION_H

/* Native Oreslang stdlib scheduler substrate, independent of Java, pthread
 * mutexes, heap allocators, and OS waits. Not a guest-callable raw release ABI.
 *
 * One successful request creates a scoped linear ticket. start/end describe
 * PHYSICAL callback execution; release/cancel may retire a granted ticket only
 * after end. The scheduler must enqueue all wake/fail tickets, never invoke
 * guest code under this gate. A busy metadata gate yields RETRY, never parks.
 */
#include "oreslang/runtime/OresCoreSync.h"
#include <stdint.h>
#include <stddef.h>

#define ORES_STD_SYNC_CAPACITY 64u

typedef enum ores_std_sync_code {
    ORES_STD_SYNC_GRANTED = 0,
    ORES_STD_SYNC_QUEUED = 1,
    ORES_STD_SYNC_RETRY = 2,
    ORES_STD_SYNC_FULL = 3,
    ORES_STD_SYNC_STALE = 4,
    ORES_STD_SYNC_EXECUTING = 5,
    ORES_STD_SYNC_EXHAUSTED = 6,
    ORES_STD_SYNC_INVALID = 7,
    ORES_STD_SYNC_CLOSED = 8
} ores_std_sync_code;

typedef enum ores_std_sync_kind {
    ORES_STD_MUTEX = 1,
    ORES_STD_RW = 2
} ores_std_sync_kind;

typedef enum ores_std_access {
    ORES_STD_READ = 1,
    ORES_STD_WRITE = 2
} ores_std_access;

typedef struct ores_std_sync_gate ores_std_sync_gate;
typedef struct ores_std_ticket {
    const ores_std_sync_gate* scope;
    uint64_t scope_generation;
    uint64_t sequence;
} ores_std_ticket;

typedef enum ores_std_slot_state {
    ORES_STD_FREE = 0,
    ORES_STD_WAITING = 1,
    ORES_STD_ADMITTED = 2,
    ORES_STD_RUNNING = 3
} ores_std_slot_state;

typedef struct ores_std_slot {
    ores_std_ticket ticket;
    unsigned int native_epoch;
    ores_std_access access;
    ores_std_slot_state state;
} ores_std_slot;

struct ores_std_sync_gate {
    atomic_flag metadata;
    ores_std_sync_kind kind;
    ores_core_mutex_gate mutex;
    ores_core_rw_gate rw;
    uint64_t scope_generation; /* trusted nonzero incarnation, never reused at this address */
    uint64_t next_sequence;
    bool exhausted;
    bool sealed;
    ores_std_slot slots[ORES_STD_SYNC_CAPACITY];
};

#define ORES_STD_MUTEX_INIT { .metadata=ATOMIC_FLAG_INIT, .kind=ORES_STD_MUTEX, \
    .mutex=ORES_CORE_MUTEX_GATE_INIT, .rw=ORES_CORE_RW_GATE_INIT }
#define ORES_STD_RW_INIT { .metadata=ATOMIC_FLAG_INIT, .kind=ORES_STD_RW, \
    .mutex=ORES_CORE_MUTEX_GATE_INIT, .rw=ORES_CORE_RW_GATE_INIT }
#define ORES_STD_MUTEX_INIT_SCOPE(GEN) { .metadata=ATOMIC_FLAG_INIT, \
    .kind=ORES_STD_MUTEX, .mutex=ORES_CORE_MUTEX_GATE_INIT, \
    .rw=ORES_CORE_RW_GATE_INIT, .scope_generation=(GEN) }
#define ORES_STD_RW_INIT_SCOPE(GEN) { .metadata=ATOMIC_FLAG_INIT, \
    .kind=ORES_STD_RW, .mutex=ORES_CORE_MUTEX_GATE_INIT, \
    .rw=ORES_CORE_RW_GATE_INIT, .scope_generation=(GEN) }

/* Notifications contain only opaque tickets, never callbacks or payloads.
 * An exhausted native generation fails all queued tickets explicitly. */
typedef struct ores_std_sync_result {
    ores_std_sync_code code;
    ores_std_ticket ticket;
    unsigned int admitted_count;
    ores_std_ticket admitted[ORES_STD_SYNC_CAPACITY];
    unsigned int failed_count;
    ores_std_ticket failed[ORES_STD_SYNC_CAPACITY];
} ores_std_sync_result;

static inline ores_std_sync_result ores_std_empty(ores_std_sync_code code) {
    ores_std_sync_result out = {0};
    out.code = code;
    return out;
}
static inline bool ores_std_enter_metadata(ores_std_sync_gate* gate) {
    return !atomic_flag_test_and_set_explicit(&gate->metadata, memory_order_acquire);
}
static inline void ores_std_leave_metadata(ores_std_sync_gate* gate) {
    atomic_flag_clear_explicit(&gate->metadata, memory_order_release);
}
/* A freestanding embedding/scheduler must provide a new nonzero generation
 * on each allocation/restart, including when the address is reused. The gate
 * cannot mint this identity from an OS clock or random generator. */
static inline ores_std_sync_code ores_std_bind_scope(
        ores_std_sync_gate* gate, uint64_t trusted_generation) {
    if (!gate || !trusted_generation) return ORES_STD_SYNC_INVALID;
    if (!ores_std_enter_metadata(gate)) return ORES_STD_SYNC_RETRY;
    ores_std_sync_code code = ORES_STD_SYNC_INVALID;
    if (gate->scope_generation == trusted_generation) {
        code = ORES_STD_SYNC_GRANTED;
    } else if (gate->scope_generation == 0 && !gate->next_sequence &&
               !gate->sealed && !gate->exhausted) {
        gate->scope_generation = trusted_generation;
        code = ORES_STD_SYNC_GRANTED;
    }
    ores_std_leave_metadata(gate);
    return code;
}
static inline ores_std_slot* ores_std_find(ores_std_sync_gate* gate, ores_std_ticket ticket) {
    if (ticket.scope != gate || ticket.scope_generation == 0 ||
        ticket.scope_generation != gate->scope_generation || ticket.sequence == 0)
        return NULL;
    for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; i++)
        if (gate->slots[i].state != ORES_STD_FREE &&
            gate->slots[i].ticket.sequence == ticket.sequence)
            return &gate->slots[i];
    return NULL;
}
static inline ores_std_slot* ores_std_free_slot(ores_std_sync_gate* gate) {
    for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; i++)
        if (gate->slots[i].state == ORES_STD_FREE) return &gate->slots[i];
    return NULL;
}
static inline ores_std_slot* ores_std_first_waiter(ores_std_sync_gate* gate) {
    ores_std_slot* earliest = NULL;
    for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; i++) {
        ores_std_slot* candidate = &gate->slots[i];
        if (candidate->state == ORES_STD_WAITING &&
            (!earliest || candidate->ticket.sequence < earliest->ticket.sequence))
            earliest = candidate;
    }
    return earliest;
}
static inline bool ores_std_has_active_writer(ores_std_sync_gate* gate) {
    for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; i++) {
        ores_std_slot* candidate = &gate->slots[i];
        if ((candidate->state == ORES_STD_ADMITTED ||
             candidate->state == ORES_STD_RUNNING) &&
            candidate->access == ORES_STD_WRITE) return true;
    }
    return false;
}
static inline bool ores_std_has_any_active(ores_std_sync_gate* gate) {
    for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; i++)
        if (gate->slots[i].state == ORES_STD_ADMITTED ||
            gate->slots[i].state == ORES_STD_RUNNING) return true;
    return false;
}
static inline ores_core_sync_result ores_std_native_admit(
        ores_std_sync_gate* gate, ores_std_slot* slot) {
    unsigned int epoch = 0;
    ores_core_sync_result code;
    if (gate->kind == ORES_STD_MUTEX) {
        code = ores_core_mutex_try_acquire(&gate->mutex, &epoch);
    } else if (slot->access == ORES_STD_READ) {
        code = ores_core_rw_try_read(&gate->rw);
    } else {
        code = ores_core_rw_try_write(&gate->rw, NULL, &epoch);
    }
    if (code == ORES_CORE_SYNC_OK) {
        slot->native_epoch = epoch;
        slot->state = ORES_STD_ADMITTED;
    }
    return code;
}
static inline ores_core_sync_result ores_std_native_release(
        ores_std_sync_gate* gate, ores_std_slot* slot) {
    if (gate->kind == ORES_STD_MUTEX)
        return ores_core_mutex_release(&gate->mutex, slot->native_epoch);
    if (slot->access == ORES_STD_READ)
        return ores_core_rw_read_release(&gate->rw);
    return ores_core_rw_write_release(&gate->rw, slot->native_epoch);
}
/* Called only while holding metadata. No queued reader may jump ahead of a
 * previously queued writer; an initial batch of queued readers may overlap. */
static inline void ores_std_promote(ores_std_sync_gate* gate, ores_std_sync_result* out) {
    if (gate->exhausted || gate->sealed) return;
    for (;;) {
        ores_std_slot* next = ores_std_first_waiter(gate);
        if (!next || ores_std_has_active_writer(gate)) return;
        if (next->access == ORES_STD_WRITE && ores_std_has_any_active(gate)) return;
        ores_core_sync_result rc = ores_std_native_admit(gate, next);
        if (rc == ORES_CORE_SYNC_BUSY) return;
        if (rc == ORES_CORE_SYNC_EXHAUSTED) {
            gate->exhausted = true;
            for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; i++) {
                ores_std_slot* slot = &gate->slots[i];
                if (slot->state == ORES_STD_WAITING) {
                    out->failed[out->failed_count++] = slot->ticket;
                    slot->state = ORES_STD_FREE;
                }
            }
            return;
        }
        if (rc != ORES_CORE_SYNC_OK) return; /* fail closed on native error */
        out->admitted[out->admitted_count++] = next->ticket;
        if (next->access == ORES_STD_WRITE) return;
    }
}
static inline ores_std_sync_result ores_std_request(
        ores_std_sync_gate* gate, ores_std_access access) {
    if (!gate || (gate->kind != ORES_STD_MUTEX && gate->kind != ORES_STD_RW) ||
        (access != ORES_STD_READ && access != ORES_STD_WRITE) ||
        (gate->kind == ORES_STD_MUTEX && access != ORES_STD_WRITE))
        return ores_std_empty(ORES_STD_SYNC_INVALID);
    if (!ores_std_enter_metadata(gate)) return ores_std_empty(ORES_STD_SYNC_RETRY);
    ores_std_sync_result out = ores_std_empty(ORES_STD_SYNC_QUEUED);
    ores_std_slot* slot = ores_std_free_slot(gate);
    if (gate->scope_generation == 0) {
        out.code = ORES_STD_SYNC_INVALID;
    } else if (gate->sealed) {
        out.code = ORES_STD_SYNC_CLOSED;
    } else if (gate->exhausted || gate->next_sequence == UINT64_MAX) {
        out.code = ORES_STD_SYNC_EXHAUSTED;
    } else if (!slot) {
        out.code = ORES_STD_SYNC_FULL;
    } else {
        slot->ticket.scope = gate;
        slot->ticket.scope_generation = gate->scope_generation;
        slot->ticket.sequence = ++gate->next_sequence;
        slot->access = access;
        slot->state = ORES_STD_WAITING;
        slot->native_epoch = 0u;
        out.ticket = slot->ticket;
        /* Fairness: do not leapfrog older waiting tickets. */
        if (ores_std_first_waiter(gate) == slot &&
            !ores_std_has_active_writer(gate) &&
            (access == ORES_STD_READ || !ores_std_has_any_active(gate))) {
            ores_core_sync_result rc = ores_std_native_admit(gate, slot);
            if (rc == ORES_CORE_SYNC_OK) out.code = ORES_STD_SYNC_GRANTED;
            else if (rc == ORES_CORE_SYNC_EXHAUSTED) {
                gate->exhausted = true;
                slot->state = ORES_STD_FREE;
                out.code = ORES_STD_SYNC_EXHAUSTED;
            }
        }
    }
    ores_std_leave_metadata(gate);
    return out;
}
/* Trusted scheduler marks a physical guest callback's entry/exit. It must
 * call leave in finally/unwind and release only AFTER physical quiescence. */
static inline ores_std_sync_code ores_std_start(
        ores_std_sync_gate* gate, ores_std_ticket ticket) {
    if (!gate) return ORES_STD_SYNC_INVALID;
    if (!ores_std_enter_metadata(gate)) return ORES_STD_SYNC_RETRY;
    ores_std_slot* slot = ores_std_find(gate, ticket);
    ores_std_sync_code code = ORES_STD_SYNC_STALE;
    if (slot) {
        code = (gate->sealed && slot->state == ORES_STD_ADMITTED)
            ? ORES_STD_SYNC_CLOSED :
            slot->state == ORES_STD_ADMITTED
            ? ORES_STD_SYNC_GRANTED :
            slot->state == ORES_STD_WAITING ? ORES_STD_SYNC_QUEUED :
            ORES_STD_SYNC_EXECUTING;
        /* Closed is an observation only: never turn a revoked dormant
         * admission into a physical callback. STOP owns revocation. */
        if (!gate->sealed && slot->state == ORES_STD_ADMITTED)
            slot->state = ORES_STD_RUNNING;
    }
    ores_std_leave_metadata(gate);
    return code;
}
static inline ores_std_sync_code ores_std_end(
        ores_std_sync_gate* gate, ores_std_ticket ticket) {
    if (!gate) return ORES_STD_SYNC_INVALID;
    if (!ores_std_enter_metadata(gate)) return ORES_STD_SYNC_RETRY;
    ores_std_slot* slot = ores_std_find(gate, ticket);
    ores_std_sync_code code = ORES_STD_SYNC_STALE;
    if (slot && slot->state == ORES_STD_RUNNING) {
        slot->state = ORES_STD_ADMITTED;
        code = ORES_STD_SYNC_GRANTED;
    }
    ores_std_leave_metadata(gate);
    return code;
}
/* release and cancel have the same resource retirement after callback end.
 * A queued cancellation drops its ticket without touching native state.
 * The returned cohort is the exact set of continuations to wake. */
static inline ores_std_sync_result ores_std_release(
        ores_std_sync_gate* gate, ores_std_ticket ticket) {
    if (!gate) return ores_std_empty(ORES_STD_SYNC_INVALID);
    if (!ores_std_enter_metadata(gate)) return ores_std_empty(ORES_STD_SYNC_RETRY);
    ores_std_sync_result out = ores_std_empty(ORES_STD_SYNC_STALE);
    ores_std_slot* slot = ores_std_find(gate, ticket);
    if (slot) {
        if (slot->state == ORES_STD_RUNNING) out.code = ORES_STD_SYNC_EXECUTING;
        else if (slot->state == ORES_STD_WAITING) {
            slot->state = ORES_STD_FREE;
            out.code = ORES_STD_SYNC_GRANTED;
            ores_std_promote(gate, &out);
        } else if (slot->state == ORES_STD_ADMITTED) {
            if (ores_std_native_release(gate, slot) == ORES_CORE_SYNC_OK) {
                slot->state = ORES_STD_FREE;
                out.code = ORES_STD_SYNC_GRANTED;
                ores_std_promote(gate, &out);
            }
        }
    }
    ores_std_leave_metadata(gate);
    return out;
}
/* Supervisor STOP/PANIC: seal once, drain queued work and revoke dormant
 * grants. A running callback retains its native borrow until it ends and
 * releases normally. Scheduler dispatches all failed ticket results outside
 * the metadata lock, then checks physical quiescence before teardown.
 */
static inline ores_std_sync_result ores_std_seal(ores_std_sync_gate* gate) {
    if (!gate) return ores_std_empty(ORES_STD_SYNC_INVALID);
    if (!ores_std_enter_metadata(gate)) return ores_std_empty(ORES_STD_SYNC_RETRY);
    ores_std_sync_result out = ores_std_empty(ORES_STD_SYNC_GRANTED);
    if (gate->sealed) {
        out.code = ORES_STD_SYNC_CLOSED;
    } else {
        gate->sealed = true;
        for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; i++) {
            ores_std_slot* slot = &gate->slots[i];
            if (slot->state == ORES_STD_WAITING ||
                slot->state == ORES_STD_ADMITTED) {
                bool good = slot->state == ORES_STD_WAITING ||
                    ores_std_native_release(gate, slot) == ORES_CORE_SYNC_OK;
                if (good) {
                    out.failed[out.failed_count++] = slot->ticket;
                    slot->state = ORES_STD_FREE;
                }
            }
        }
    }
    ores_std_leave_metadata(gate);
    return out;
}
/* No guest callback can be scheduled or reclaimed from a stale bool:
 * physical callback retirement is a separate state from Future settlement. */
static inline ores_std_sync_code ores_std_quiescent(ores_std_sync_gate* gate) {
    if (!gate) return ORES_STD_SYNC_INVALID;
    if (!ores_std_enter_metadata(gate)) return ORES_STD_SYNC_RETRY;
    bool live = ores_std_has_any_active(gate) || ores_std_first_waiter(gate);
    ores_std_sync_code code = (!live && gate->sealed)
        ? ORES_STD_SYNC_GRANTED : ORES_STD_SYNC_EXECUTING;
    ores_std_leave_metadata(gate);
    return code;
}
/* Suspend gate for the same scoped logical continuation. If a ticket has
 * any live admission, it still carries an active loan and cannot suspend. */
static inline ores_std_sync_code ores_std_ready_to_suspend(
        ores_std_sync_gate* gate, ores_std_ticket ticket) {
    if (!gate || ticket.scope != gate || !ticket.sequence ||
        !ticket.scope_generation || ticket.scope_generation != gate->scope_generation)
        return ORES_STD_SYNC_INVALID;
    if (!ores_std_enter_metadata(gate)) return ORES_STD_SYNC_RETRY;
    ores_std_sync_code code = ores_std_find(gate, ticket)
        ? ORES_STD_SYNC_EXECUTING : ORES_STD_SYNC_GRANTED;
    ores_std_leave_metadata(gate);
    return code;
}
#endif
