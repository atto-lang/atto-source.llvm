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
    ORES_NATIVE_SELECT_ARM_COMMITTING = 3,
    ORES_NATIVE_SELECT_ARM_COMMITTED = 4,
    ORES_NATIVE_SELECT_ARM_DETACHED = 5
} OresNativeSelectArmState;

typedef struct OresNativeSelectArbiter {
    atomic_uint_fast64_t generation;
    atomic_uint_fast64_t winner;
    atomic_size_t active_tokens;
    size_t arm_count;
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
    arbiter->arm_count = arm_count;
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
                &arbiter->generation, memory_order_acquire) == generation)
        return 0;
    arbiter->arm_count = arm_count;
    atomic_store_explicit(
        &arbiter->generation, generation, memory_order_release);
    atomic_store_explicit(
        &arbiter->winner, UINT64_C(0), memory_order_release);
    for (size_t i = 0; i < ORES_NATIVE_SELECT_MAX_ARMS; ++i)
        atomic_store_explicit(
            &arbiter->arms[i].state,
            ORES_NATIVE_SELECT_ARM_EMPTY,
            memory_order_release);
    return 1;
}

static inline int ores_native_select_register(
        OresNativeSelectArbiter* arbiter,
        uint64_t arm,
        OresNativeSelectToken* out) {
    if (out) *out = (OresNativeSelectToken){0};
    if (!arbiter || !out || arm >= arbiter->arm_count
            || atomic_load_explicit(
                &arbiter->winner, memory_order_acquire) != 0)
        return 0;

    unsigned expected = ORES_NATIVE_SELECT_ARM_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
            &arbiter->arms[arm].state,
            &expected,
            ORES_NATIVE_SELECT_ARM_REGISTERING,
            memory_order_acq_rel,
            memory_order_acquire))
        return 0;

    const uint64_t selection_generation = atomic_load_explicit(
        &arbiter->generation, memory_order_acquire);
    uint_fast64_t arm_generation = atomic_load_explicit(
        &arbiter->arms[arm].generation, memory_order_relaxed);
    if (!selection_generation || selection_generation == UINT64_MAX
            || arm_generation == UINT64_MAX) {
        atomic_store_explicit(
            &arbiter->arms[arm].state,
            ORES_NATIVE_SELECT_ARM_EMPTY,
            memory_order_release);
        return 0;
    }
    ++arm_generation;
    atomic_store_explicit(
        &arbiter->arms[arm].generation,
        arm_generation,
        memory_order_release);
    atomic_fetch_add_explicit(
        &arbiter->active_tokens, (size_t)1, memory_order_acq_rel);
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
            &arbiter->winner, memory_order_acquire) != 0) {
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
            || token.arm >= token.arbiter->arm_count
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
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

static inline OresNativeSelectCommitResult ores_native_select_try_commit(
        OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || token.arm >= token.arbiter->arm_count
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
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
    return winner == UINT64_MAX
        ? ORES_NATIVE_SELECT_SEALED
        : ORES_NATIVE_SELECT_LOST;
}

static inline int ores_native_select_detach(OresNativeSelectToken token) {
    if (!token.arbiter || !token.selection_generation
            || !token.arm_generation
            || token.arm >= token.arbiter->arm_count
            || atomic_load_explicit(
                &token.arbiter->generation, memory_order_acquire)
                != token.selection_generation
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

static inline int ores_native_select_seal(OresNativeSelectArbiter* arbiter) {
    if (!arbiter) return 0;
    uint64_t expected = 0;
    const int sealed_before_winner = atomic_compare_exchange_strong_explicit(
        &arbiter->winner,
        &expected,
        UINT64_MAX,
        memory_order_acq_rel,
        memory_order_acquire);
    for (size_t i = 0; i < arbiter->arm_count; ++i) {
        unsigned state = ORES_NATIVE_SELECT_ARM_REGISTERED;
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
    if (!winner || winner == UINT64_MAX) return 0;
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
