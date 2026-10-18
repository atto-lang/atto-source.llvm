#ifndef ORESLANG_CORE_SYNC_H
#define ORESLANG_CORE_SYNC_H

/* Freestanding core synchronization substrate (C11): no allocator, threads,
 * pthreads, OS waits, or stdlib dependency. NOT a guest-callable raw unlock API.
 * The compiler and trusted scheduler own linear guard/continuation authority.
 * No operation parks or spins waiting for another owner.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <limits.h>

#if ATOMIC_INT_LOCK_FREE != 2
#error "OresCoreSync requires always-lock-free unsigned int atomics; use an explicit target atomic provider"
#endif
#if UINT_MAX != 0xffffffffu
#error "OresCoreSync requires a 32-bit unsigned int atomic word"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ores_core_sync_result {
    ORES_CORE_SYNC_OK = 0,
    ORES_CORE_SYNC_BUSY = 1,
    ORES_CORE_SYNC_STALE = 2,
    ORES_CORE_SYNC_EXHAUSTED = 3,
    ORES_CORE_SYNC_INVALID = 4
} ores_core_sync_result;

/* Host-only linear capability: never give guest code direct authority to copy,
 * forge or release these integers. Zero is not a token. Generation wrap fails.
 */
typedef struct ores_core_mutex_gate {
    atomic_uint owner;
    unsigned int issued; /* writable only under owner==UINT_MAX reservation */
} ores_core_mutex_gate;

#define ORES_CORE_MUTEX_GATE_INIT { ATOMIC_VAR_INIT(0u), 0u }

static inline ores_core_sync_result
ores_core_mutex_try_acquire(ores_core_mutex_gate* gate, unsigned int* token) {
    if (!gate || !token) return ORES_CORE_SYNC_INVALID;
    unsigned int empty = 0u;
    if (!atomic_compare_exchange_strong_explicit(&gate->owner, &empty,
            UINT_MAX, memory_order_acq_rel, memory_order_acquire)) {
        return ORES_CORE_SYNC_BUSY;
    }
    if (gate->issued == UINT_MAX - 1u) {
        atomic_store_explicit(&gate->owner, 0u, memory_order_release);
        return ORES_CORE_SYNC_EXHAUSTED;
    }
    unsigned int granted = ++gate->issued;
    atomic_store_explicit(&gate->owner, granted, memory_order_release);
    *token = granted;
    return ORES_CORE_SYNC_OK;
}

static inline ores_core_sync_result
ores_core_mutex_release(ores_core_mutex_gate* gate, unsigned int token) {
    if (!gate || token == 0u || token == UINT_MAX) return ORES_CORE_SYNC_INVALID;
    unsigned int expected = token;
    return atomic_compare_exchange_strong_explicit(&gate->owner, &expected,
        0u, memory_order_release, memory_order_relaxed)
        ? ORES_CORE_SYNC_OK : ORES_CORE_SYNC_STALE;
}

/* Bit 31 encodes writer; bits 0..30 encode reader count or write epoch.
 * 0xffffffff is reserved as an unpublished writer-epoch transition.
 * A waiter reservation gives waiting writers preference over new readers.
 * No fairness among queued writers is promised by this core gate: stdlib's
 * scheduler provides bounded FIFO admission and cancellation ordering.
 */
#define ORES_CORE_RW_WRITE_BIT 0x80000000u
#define ORES_CORE_RW_MAX_READERS 0x7ffffffeu
#define ORES_CORE_RW_RESERVED 0xffffffffu

typedef struct ores_core_rw_gate {
    atomic_uint state;
    atomic_uint waiting_writers;
    unsigned int issued_writers; /* only under state==RESERVED */
} ores_core_rw_gate;

#define ORES_CORE_RW_GATE_INIT { ATOMIC_VAR_INIT(0u), ATOMIC_VAR_INIT(0u), 0u }

typedef struct ores_core_rw_writer_wait {
    atomic_uint active; /* 0=idle, 1=queued, 2=transition, 3=claiming, 4=granted */
    ores_core_rw_gate* gate;
} ores_core_rw_writer_wait;

#define ORES_CORE_RW_WRITER_WAIT_INIT { ATOMIC_VAR_INIT(0u), 0 }

static inline ores_core_sync_result
ores_core_rw_writer_register(ores_core_rw_gate* gate, ores_core_rw_writer_wait* wait) {
    if (!gate || !wait) return ORES_CORE_SYNC_INVALID;
    unsigned int zero = 0u;
    if (!atomic_compare_exchange_strong_explicit(&wait->active, &zero, 2u,
            memory_order_acq_rel, memory_order_acquire)) return ORES_CORE_SYNC_BUSY;
    /* Only the registering owner writes wait->gate. Every later access is
     * after publication of active=1 with release semantics. */
    wait->gate = gate;
    unsigned int pending = atomic_load_explicit(&gate->waiting_writers, memory_order_relaxed);
    for (;;) {
        if (pending == UINT_MAX) {
            atomic_store_explicit(&wait->active, 0u, memory_order_release);
            return ORES_CORE_SYNC_EXHAUSTED;
        }
        if (atomic_compare_exchange_weak_explicit(&gate->waiting_writers,
                &pending, pending + 1u, memory_order_acq_rel, memory_order_relaxed)) break;
    }
    atomic_store_explicit(&wait->active, 1u, memory_order_release);
    return ORES_CORE_SYNC_OK;
}

static inline ores_core_sync_result
ores_core_rw_writer_unregister(ores_core_rw_writer_wait* wait) {
    if (!wait) return ORES_CORE_SYNC_INVALID;
    /* Cancellation cannot revoke a grant while the writer CAS is in flight.
     * State 3 claims the wait; state 4 publishes the completed grant. */
    unsigned int current = atomic_load_explicit(&wait->active, memory_order_acquire);
    for (;;) {
        if (current != 1u && current != 4u) return ORES_CORE_SYNC_STALE;
        if (atomic_compare_exchange_weak_explicit(&wait->active, &current, 2u,
                memory_order_acq_rel, memory_order_acquire)) break;
    }
    /* There is exactly one matching pending reservation at this point. */
    atomic_fetch_sub_explicit(&wait->gate->waiting_writers, 1u, memory_order_acq_rel);
    atomic_store_explicit(&wait->active, 0u, memory_order_release);
    return ORES_CORE_SYNC_OK;
}

static inline ores_core_sync_result
ores_core_rw_try_read(ores_core_rw_gate* gate) {
    if (!gate) return ORES_CORE_SYNC_INVALID;
    if (atomic_load_explicit(&gate->waiting_writers, memory_order_acquire))
        return ORES_CORE_SYNC_BUSY;
    unsigned int old = atomic_load_explicit(&gate->state, memory_order_relaxed);
    for (;;) {
        if ((old & ORES_CORE_RW_WRITE_BIT) || old >= ORES_CORE_RW_MAX_READERS)
            return ORES_CORE_SYNC_BUSY;
        if (atomic_compare_exchange_weak_explicit(&gate->state, &old, old+1u,
                memory_order_acquire, memory_order_relaxed)) break;
        if (atomic_load_explicit(&gate->waiting_writers, memory_order_acquire))
            return ORES_CORE_SYNC_BUSY;
    }
    /* Writer registration racing admission cannot be ignored: back out the
     * newly acquired reader to avoid overtaking queued writers. */
    if (atomic_load_explicit(&gate->waiting_writers, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&gate->state, 1u, memory_order_release);
        return ORES_CORE_SYNC_BUSY;
    }
    return ORES_CORE_SYNC_OK;
}

/* Caller must hold a verified live reader loan. Guest code cannot invoke
 * this raw C ABI: duplicate release otherwise could revoke another reader. */
static inline ores_core_sync_result
ores_core_rw_read_release(ores_core_rw_gate* gate) {
    if (!gate) return ORES_CORE_SYNC_INVALID;
    unsigned int old = atomic_load_explicit(&gate->state, memory_order_relaxed);
    for (;;) {
        if (!old || (old & ORES_CORE_RW_WRITE_BIT)) return ORES_CORE_SYNC_STALE;
        if (atomic_compare_exchange_weak_explicit(&gate->state, &old, old-1u,
                memory_order_release, memory_order_relaxed)) return ORES_CORE_SYNC_OK;
    }
}

static inline ores_core_sync_result
ores_core_rw_try_write(ores_core_rw_gate* gate,
        ores_core_rw_writer_wait* registered_wait, unsigned int* token) {
    if (!gate || !token) return ORES_CORE_SYNC_INVALID;
    if (registered_wait) {
        unsigned int queued = 1u;
        if (!atomic_compare_exchange_strong_explicit(&registered_wait->active,
                &queued, 3u, memory_order_acq_rel, memory_order_acquire))
            return ORES_CORE_SYNC_BUSY;
        if (registered_wait->gate != gate) {
            atomic_store_explicit(&registered_wait->active, 1u, memory_order_release);
            return ORES_CORE_SYNC_INVALID;
        }
    } else if (atomic_load_explicit(&gate->waiting_writers, memory_order_acquire)) {
        return ORES_CORE_SYNC_BUSY;
    }
    unsigned int zero = 0u;
    if (!atomic_compare_exchange_strong_explicit(&gate->state, &zero,
            ORES_CORE_RW_RESERVED, memory_order_acq_rel, memory_order_acquire)) {
        if (registered_wait)
            atomic_store_explicit(&registered_wait->active, 1u, memory_order_release);
        return ORES_CORE_SYNC_BUSY;
    }
    if (gate->issued_writers == ORES_CORE_RW_MAX_READERS) {
        atomic_store_explicit(&gate->state, 0u, memory_order_release);
        if (registered_wait)
            atomic_store_explicit(&registered_wait->active, 1u, memory_order_release);
        return ORES_CORE_SYNC_EXHAUSTED;
    }
    const unsigned int next = ++gate->issued_writers;
    atomic_store_explicit(&gate->state, ORES_CORE_RW_WRITE_BIT | next,
        memory_order_release);
    *token = next;
    if (registered_wait)
        atomic_store_explicit(&registered_wait->active, 4u, memory_order_release);
    return ORES_CORE_SYNC_OK;
}

static inline ores_core_sync_result
ores_core_rw_write_release(ores_core_rw_gate* gate, unsigned int token) {
    if (!gate || !token || token > ORES_CORE_RW_MAX_READERS)
        return ORES_CORE_SYNC_INVALID;
    unsigned int expected = ORES_CORE_RW_WRITE_BIT | token;
    return atomic_compare_exchange_strong_explicit(&gate->state, &expected, 0u,
        memory_order_release, memory_order_relaxed)
        ? ORES_CORE_SYNC_OK : ORES_CORE_SYNC_STALE;
}

#ifdef __cplusplus
}
#endif
#endif
