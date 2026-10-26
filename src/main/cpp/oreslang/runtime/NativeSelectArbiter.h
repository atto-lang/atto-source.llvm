#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "oreslang/runtime/NativeSelectToken.h"

/*
 * One bounded, one-shot heterogeneous select arbitration cell.
 *
 * The owner keeps this object in runtime-owned storage until active_tokens is
 * zero. Sources keep only OresNativeSelectToken values; no source callback may
 * execute guest code while committing this cell.
 *
 * winner:
 *   0          => OPEN
 *   UINT64_MAX => SEALED with no winner
 *   arm + 1    => committed winner
 */
enum { ORES_NATIVE_SELECT_MAX_ARMS = 64 };
#define ORES_NATIVE_SELECT_WINNER_SEALED UINT64_MAX
#define ORES_NATIVE_SELECT_WINNER_REARMING (UINT64_MAX - UINT64_C(1))

typedef enum {
    ORES_NATIVE_SELECT_INVALID = 0,
    ORES_NATIVE_SELECT_WON = 1,
    ORES_NATIVE_SELECT_LOST = 2,
    ORES_NATIVE_SELECT_SEALED = 3
} OresNativeSelectCommitResult;

typedef enum {
    ORES_NATIVE_SELECT_ARM_EMPTY = 0,
    ORES_NATIVE_SELECT_ARM_REGISTERING = 1,
    ORES_NATIVE_SELECT_ARM_REGISTERED = 2,
    /* Ownership transferred to one runtime source implementation. Generic
     * coordinator commit/detach APIs no longer have authority over this arm. */
    ORES_NATIVE_SELECT_ARM_BOUND = 3,
    ORES_NATIVE_SELECT_ARM_COMMITTING = 4,
    ORES_NATIVE_SELECT_ARM_COMMITTED = 5,
    ORES_NATIVE_SELECT_ARM_DETACHED = 6
} OresNativeSelectArmState;

typedef struct OresNativeSelectArbiter {
    atomic_uint_fast64_t generation;
    atomic_uint_fast64_t winner;
    atomic_size_t active_tokens;
    /* Physical continuation/wake loans fence generation reuse even after
     * every source registration token has logically retired. */
    atomic_size_t active_physical_loans;
    atomic_size_t arm_count;
    struct {
        atomic_uint_fast64_t generation;
        atomic_uint state;
    } arms[ORES_NATIVE_SELECT_MAX_ARMS];
} OresNativeSelectArbiter;

static inline int ores_native_select_arbiter_init(
        OresNativeSelectArbiter* arbiter,
        uint64_t generation,
        size_t arm_count) {
    if (!arbiter || !generation || generation == UINT64_MAX
            || !arm_count || arm_count > ORES_NATIVE_SELECT_MAX_ARMS)
        return 0;
    atomic_init(&arbiter->generation, generation);
    atomic_init(&arbiter->winner, UINT64_C(0));
    atomic_init(&arbiter->active_tokens, (size_t)0);
    atomic_init(&arbiter->active_physical_loans, (size_t)0);
    atomic_init(&arbiter->arm_count, arm_count);
    for (size_t i = 0; i < ORES_NATIVE_SELECT_MAX_ARMS; ++i) {
        atomic_init(&arbiter->arms[i].generation, UINT64_C(0));
        atomic_init(&arbiter->arms[i].state, ORES_NATIVE_SELECT_ARM_EMPTY);
    }
    return 1;
}

static inline int ores_native_select_arbiter_rearm(
        OresNativeSelectArbiter* arbiter,
        uint64_t generation,
        size_t arm_count) {
    if (!arbiter || !generation || generation == UINT64_MAX
            || !arm_count || arm_count > ORES_NATIVE_SELECT_MAX_ARMS
            || atomic_load_explicit(
                &arbiter->active_tokens, memory_order_acquire) != 0
            || atomic_load_explicit(
                &arbiter->active_physical_loans, memory_order_acquire) != 0)
        return 0;
    const uint64_t prior_generation = atomic_load_explicit(
        &arbiter->generation, memory_order_acquire);
    /*
     * Selection generations are capabilities, not caller labels. Never allow
     * an older value to be reused after it may have escaped in a stale token.
     * Exhaustion is therefore fail-closed rather than wrap/recycle.
     */
    if (!prior_generation || generation <= prior_generation)
        return 0;

    /*
     * A new generation may start only after the previous operation is
     * terminal. Claim a private REARMING sentinel before touching arm state;
     * registration first observes winner and therefore cannot enter while
     * reset is in progress.
     *
     * Rearm is still owner/finalizer authority: the owner calls it only after
     * the prior commit/seal routine itself has returned.
     */
    uint_fast64_t terminal = atomic_load_explicit(
        &arbiter->winner, memory_order_acquire);
    if (terminal == 0
            || terminal == ORES_NATIVE_SELECT_WINNER_REARMING)
        return 0;
    if (!atomic_compare_exchange_strong_explicit(
            &arbiter->winner,
            &terminal,
            (uint_fast64_t)ORES_NATIVE_SELECT_WINNER_REARMING,
            memory_order_acq_rel,
            memory_order_acquire))
        return 0;
    if (atomic_load_explicit(
            &arbiter->active_tokens, memory_order_acquire) != 0
            || atomic_load_explicit(
                &arbiter->active_physical_loans, memory_order_acquire) != 0) {
        atomic_store_explicit(
            &arbiter->winner, terminal, memory_order_release);
        return 0;
    }

    atomic_store_explicit(
        &arbiter->arm_count, arm_count, memory_order_relaxed);
    atomic_store_explicit(
        &arbiter->generation, generation, memory_order_relaxed);
    for (size_t i = 0; i < ORES_NATIVE_SELECT_MAX_ARMS; ++i)
        atomic_store_explicit(
            &arbiter->arms[i].state,
            ORES_NATIVE_SELECT_ARM_EMPTY,
            memory_order_relaxed);

    /* Release publication of OPEN makes the reset generation/arm metadata
     * visible to the next registering thread. */
    atomic_store_explicit(
        &arbiter->winner, UINT64_C(0), memory_order_release);
    return 1;
}

/*
 * Retain one generation-scoped physical continuation/wake loan. The retain is
 * lock-free but rechecks generation/winner after increment; rearm also checks
 * the loan count before and after claiming REARMING, so neither side can reset
 * the generation underneath an in-flight retain.
 */
static inline int ores_native_select_retain_physical_loan(
        OresNativeSelectArbiter* arbiter,
        uint64_t generation) {
    if (!arbiter || !generation || generation == UINT64_MAX
            || atomic_load_explicit(
                &arbiter->generation, memory_order_acquire) != generation
            || atomic_load_explicit(
                &arbiter->winner, memory_order_acquire) != 0)
        return 0;

    atomic_fetch_add_explicit(
        &arbiter->active_physical_loans, (size_t)1, memory_order_acq_rel);

    if (atomic_load_explicit(
            &arbiter->generation, memory_order_acquire) != generation
            || atomic_load_explicit(
                &arbiter->winner, memory_order_acquire)
                == ORES_NATIVE_SELECT_WINNER_REARMING) {
        atomic_fetch_sub_explicit(
            &arbiter->active_physical_loans, (size_t)1,
            memory_order_acq_rel);
        return 0;
    }
    return 1;
}

static inline int ores_native_select_release_physical_loan(
        OresNativeSelectArbiter* arbiter,
        uint64_t generation) {
    if (!arbiter || !generation
            || atomic_load_explicit(
                &arbiter->generation, memory_order_acquire) != generation)
        return 0;
    size_t current = atomic_load_explicit(
        &arbiter->active_physical_loans, memory_order_acquire);
    while (current != 0) {
        if (atomic_compare_exchange_weak_explicit(
                &arbiter->active_physical_loans,
                &current,
                current - 1,
                memory_order_acq_rel,
                memory_order_acquire))
            return 1;
    }
    return 0;
}

static inline size_t ores_native_select_active_physical_loans(
        const OresNativeSelectArbiter* arbiter) {
    return arbiter
        ? atomic_load_explicit(
            &arbiter->active_physical_loans, memory_order_acquire)
        : 0;
}

static inline int ores_native_select_register(
        OresNativeSelectArbiter* arbiter,
        uint64_t arm,
        OresNativeSelectToken* out) {
    if (out) *out = (OresNativeSelectToken){0};
    if (!arbiter || !out)
        return 0;

    /*
     * Acquire OPEN first. Rearm publishes generation + arm_count + reset arm
     * state before its release-store of winner=OPEN. Reading arm_count before
     * this acquire can observe the prior generation's bound and admit an arm
     * which does not exist in the newly opened generation.
     */
    if (atomic_load_explicit(
            &arbiter->winner, memory_order_acquire) != 0)
        return 0;
    const uint64_t selection_generation = atomic_load_explicit(
        &arbiter->generation, memory_order_acquire);
    const size_t selection_arm_count = atomic_load_explicit(
        &arbiter->arm_count, memory_order_acquire);
    if (!selection_generation || selection_generation == UINT64_MAX
            || arm >= selection_arm_count)
        return 0;

    unsigned expected = ORES_NATIVE_SELECT_ARM_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
            &arbiter->arms[arm].state,
            &expected,
            ORES_NATIVE_SELECT_ARM_REGISTERING,
            memory_order_acq_rel,
            memory_order_acquire))
        return 0;

    atomic_fetch_add_explicit(
        &arbiter->active_tokens, (size_t)1, memory_order_acq_rel);

    /*
     * Seal or a completed rearm may have raced the initial OPEN observation.
     * active_tokens now prevents any *new* rearm from completing until this
     * registration retires, while the generation equality rejects one which
     * linearized before our token fence became visible.
     */
    if (atomic_load_explicit(
            &arbiter->winner, memory_order_acquire) != 0
            || atomic_load_explicit(
                &arbiter->generation, memory_order_acquire)
                != selection_generation
            || arm >= atomic_load_explicit(
                &arbiter->arm_count, memory_order_acquire)) {
        atomic_store_explicit(
            &arbiter->arms[arm].state,
            ORES_NATIVE_SELECT_ARM_DETACHED,
            memory_order_release);
        atomic_fetch_sub_explicit(
            &arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
        return 0;
    }

    uint_fast64_t arm_generation = atomic_load_explicit(
        &arbiter->arms[arm].generation, memory_order_relaxed);
    if (arm_generation == UINT64_MAX) {
        atomic_store_explicit(
            &arbiter->arms[arm].state,
            ORES_NATIVE_SELECT_ARM_DETACHED,
            memory_order_release);
        atomic_fetch_sub_explicit(
            &arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
        return 0;
    }
    ++arm_generation;
    atomic_store_explicit(
        &arbiter->arms[arm].generation,
        arm_generation,
        memory_order_release);
    atomic_store_explicit(
        &arbiter->arms[arm].state,
        ORES_NATIVE_SELECT_ARM_REGISTERED,
        memory_order_release);

    *out = (OresNativeSelectToken){
        .arbiter = arbiter,
        .selection_generation = selection_generation,
        .arm_generation = arm_generation,
        .arm = arm
    };

    /* Seal/winner may have raced the final registration publication. */
    if (atomic_load_explicit(
            &arbiter->winner, memory_order_acquire) != 0
            || atomic_load_explicit(
                &arbiter->generation, memory_order_acquire)
                != selection_generation) {
        unsigned registered = ORES_NATIVE_SELECT_ARM_REGISTERED;
        if (atomic_compare_exchange_strong_explicit(
                &arbiter->arms[arm].state,
                &registered,
                ORES_NATIVE_SELECT_ARM_DETACHED,
                memory_order_acq_rel,
                memory_order_acquire))
            atomic_fetch_sub_explicit(
                &arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
        *out = (OresNativeSelectToken){0};
        return 0;
    }
    return 1;
}

static inline int ores_native_select_token_registered(
        OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
            || token.arm >= atomic_load_explicit(&token.arbiter->arm_count, memory_order_acquire)
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].generation,
                memory_order_acquire) != token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].state,
                memory_order_acquire) != ORES_NATIVE_SELECT_ARM_REGISTERED)
        return 0;
    return atomic_load_explicit(
        &token.arbiter->winner, memory_order_acquire) == 0;
}

/*
 * Transfer one registered arm into a source implementation (for example the
 * scheduler-owned async-send descriptor). After this succeeds, the caller's
 * copied token is observation-only: generic try_commit()/detach() reject it.
 *
 * The source must use the BOUND-only helpers below. This prevents a coordinator
 * or stale helper from declaring a selectable send the winner before the
 * source's real irreversible commit point (mailbox-credit publication).
 */
static inline int ores_native_select_bind_source(
        OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
            || token.arm >= atomic_load_explicit(&token.arbiter->arm_count, memory_order_acquire)
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].generation,
                memory_order_acquire) != token.arm_generation)
        return 0;

    unsigned expected = ORES_NATIVE_SELECT_ARM_REGISTERED;
    if (!atomic_compare_exchange_strong_explicit(
            &token.arbiter->arms[token.arm].state,
            &expected,
            ORES_NATIVE_SELECT_ARM_BOUND,
            memory_order_acq_rel,
            memory_order_acquire))
        return 0;

    /* A winner/seal can race the handoff. If it did, retire this token here so
     * the source never publishes work for a selection it no longer owns. */
    if (atomic_load_explicit(
            &token.arbiter->winner, memory_order_acquire) != 0
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation) {
        unsigned bound = ORES_NATIVE_SELECT_ARM_BOUND;
        if (atomic_compare_exchange_strong_explicit(
                &token.arbiter->arms[token.arm].state,
                &bound,
                ORES_NATIVE_SELECT_ARM_DETACHED,
                memory_order_acq_rel,
                memory_order_acquire))
            atomic_fetch_sub_explicit(
                &token.arbiter->active_tokens, (size_t)1,
                memory_order_acq_rel);
        return 0;
    }
    return 1;
}

static inline int ores_native_select_token_bound(
        OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
            || token.arm >= atomic_load_explicit(&token.arbiter->arm_count, memory_order_acquire)
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].generation,
                memory_order_acquire) != token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].state,
                memory_order_acquire) != ORES_NATIVE_SELECT_ARM_BOUND)
        return 0;
    return atomic_load_explicit(
        &token.arbiter->winner, memory_order_acquire) == 0;
}

static inline OresNativeSelectCommitResult
ores_native_select_try_commit_bound(OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
            || token.arm >= atomic_load_explicit(&token.arbiter->arm_count, memory_order_acquire)
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].generation,
                memory_order_acquire) != token.arm_generation)
        return ORES_NATIVE_SELECT_INVALID;

    unsigned expected = ORES_NATIVE_SELECT_ARM_BOUND;
    if (!atomic_compare_exchange_strong_explicit(
            &token.arbiter->arms[token.arm].state,
            &expected,
            ORES_NATIVE_SELECT_ARM_COMMITTING,
            memory_order_acq_rel,
            memory_order_acquire))
        return ORES_NATIVE_SELECT_INVALID;

    uint_fast64_t winner = 0;
    const uint_fast64_t encoded = token.arm + UINT64_C(1);
    if (atomic_compare_exchange_strong_explicit(
            &token.arbiter->winner,
            &winner,
            encoded,
            memory_order_acq_rel,
            memory_order_acquire)) {
        atomic_store_explicit(
            &token.arbiter->arms[token.arm].state,
            ORES_NATIVE_SELECT_ARM_COMMITTED,
            memory_order_release);
        atomic_fetch_sub_explicit(
            &token.arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
        return ORES_NATIVE_SELECT_WON;
    }

    atomic_store_explicit(
        &token.arbiter->arms[token.arm].state,
        ORES_NATIVE_SELECT_ARM_DETACHED,
        memory_order_release);
    atomic_fetch_sub_explicit(
        &token.arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
    return winner == ORES_NATIVE_SELECT_WINNER_SEALED
            || winner == ORES_NATIVE_SELECT_WINNER_REARMING
        ? ORES_NATIVE_SELECT_SEALED
        : ORES_NATIVE_SELECT_LOST;
}

static inline int ores_native_select_detach_bound(
        OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
            || token.arm >= atomic_load_explicit(&token.arbiter->arm_count, memory_order_acquire)
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].generation,
                memory_order_acquire) != token.arm_generation)
        return 0;
    unsigned expected = ORES_NATIVE_SELECT_ARM_BOUND;
    if (!atomic_compare_exchange_strong_explicit(
            &token.arbiter->arms[token.arm].state,
            &expected,
            ORES_NATIVE_SELECT_ARM_DETACHED,
            memory_order_acq_rel,
            memory_order_acquire))
        return 0;
    atomic_fetch_sub_explicit(
        &token.arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
    return 1;
}

static inline OresNativeSelectCommitResult ores_native_select_try_commit(
        OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
            || token.arm >= atomic_load_explicit(&token.arbiter->arm_count, memory_order_acquire)
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].generation,
                memory_order_acquire) != token.arm_generation)
        return ORES_NATIVE_SELECT_INVALID;

    unsigned expected = ORES_NATIVE_SELECT_ARM_REGISTERED;
    if (!atomic_compare_exchange_strong_explicit(
            &token.arbiter->arms[token.arm].state,
            &expected,
            ORES_NATIVE_SELECT_ARM_COMMITTING,
            memory_order_acq_rel,
            memory_order_acquire))
        return ORES_NATIVE_SELECT_INVALID;

    uint_fast64_t winner = 0;
    const uint_fast64_t encoded = token.arm + UINT64_C(1);
    if (atomic_compare_exchange_strong_explicit(
            &token.arbiter->winner,
            &winner,
            encoded,
            memory_order_acq_rel,
            memory_order_acquire)) {
        atomic_store_explicit(
            &token.arbiter->arms[token.arm].state,
            ORES_NATIVE_SELECT_ARM_COMMITTED,
            memory_order_release);
        atomic_fetch_sub_explicit(
            &token.arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
        return ORES_NATIVE_SELECT_WON;
    }

    atomic_store_explicit(
        &token.arbiter->arms[token.arm].state,
        ORES_NATIVE_SELECT_ARM_DETACHED,
        memory_order_release);
    atomic_fetch_sub_explicit(
        &token.arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
    return winner == ORES_NATIVE_SELECT_WINNER_SEALED
            || winner == ORES_NATIVE_SELECT_WINNER_REARMING
        ? ORES_NATIVE_SELECT_SEALED
        : ORES_NATIVE_SELECT_LOST;
}

static inline int ores_native_select_detach(OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
            || token.arm >= atomic_load_explicit(&token.arbiter->arm_count, memory_order_acquire)
            || atomic_load_explicit(
                &token.arbiter->arms[token.arm].generation,
                memory_order_acquire) != token.arm_generation)
        return 0;
    unsigned expected = ORES_NATIVE_SELECT_ARM_REGISTERED;
    if (!atomic_compare_exchange_strong_explicit(
            &token.arbiter->arms[token.arm].state,
            &expected,
            ORES_NATIVE_SELECT_ARM_DETACHED,
            memory_order_acq_rel,
            memory_order_acquire))
        return 0;
    atomic_fetch_sub_explicit(
        &token.arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
    return 1;
}

static inline int ores_native_select_seal(
        OresNativeSelectArbiter* arbiter,
        uint64_t generation) {
    if (!arbiter || !generation || generation == UINT64_MAX
            || atomic_load_explicit(
                &arbiter->generation, memory_order_acquire) != generation)
        return 0;

    uint_fast64_t expected = 0;
    const int sealed_before_winner = atomic_compare_exchange_strong_explicit(
        &arbiter->winner,
        &expected,
        (uint_fast64_t)ORES_NATIVE_SELECT_WINNER_SEALED,
        memory_order_acq_rel,
        memory_order_acquire);

    /*
     * If another winner was already terminal, a concurrent owner may rearm.
     * Never let a delayed seal from generation N detach registrations in N+1.
     * Active tokens fence rearm while there are actual losers to retire.
     */
    if (atomic_load_explicit(
            &arbiter->generation, memory_order_acquire) != generation)
        return 0;
    if (!sealed_before_winner
            && atomic_load_explicit(
                &arbiter->active_tokens, memory_order_acquire) == 0)
        return 0;

    const size_t arm_count = atomic_load_explicit(
        &arbiter->arm_count, memory_order_acquire);
    for (size_t i = 0; i < arm_count; ++i) {
        if (atomic_load_explicit(
                &arbiter->generation, memory_order_acquire) != generation)
            return 0;

        unsigned state = ORES_NATIVE_SELECT_ARM_REGISTERED;
        if (atomic_compare_exchange_strong_explicit(
                &arbiter->arms[i].state,
                &state,
                ORES_NATIVE_SELECT_ARM_DETACHED,
                memory_order_acq_rel,
                memory_order_acquire)) {
            atomic_fetch_sub_explicit(
                &arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
            continue;
        }
        state = ORES_NATIVE_SELECT_ARM_BOUND;
        if (atomic_compare_exchange_strong_explicit(
                &arbiter->arms[i].state,
                &state,
                ORES_NATIVE_SELECT_ARM_DETACHED,
                memory_order_acq_rel,
                memory_order_acquire))
            atomic_fetch_sub_explicit(
                &arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
    }
    return sealed_before_winner;
}

static inline int ores_native_select_winner(
        const OresNativeSelectArbiter* arbiter,
        uint64_t* arm_out) {
    if (!arbiter || !arm_out) return 0;
    const uint64_t winner = atomic_load_explicit(
        &arbiter->winner, memory_order_acquire);
    if (!winner
            || winner == ORES_NATIVE_SELECT_WINNER_SEALED
            || winner == ORES_NATIVE_SELECT_WINNER_REARMING)
        return 0;
    *arm_out = winner - UINT64_C(1);
    return 1;
}

static inline size_t ores_native_select_active_tokens(
        const OresNativeSelectArbiter* arbiter) {
    return arbiter
        ? atomic_load_explicit(
            &arbiter->active_tokens, memory_order_acquire)
        : 0;
}
