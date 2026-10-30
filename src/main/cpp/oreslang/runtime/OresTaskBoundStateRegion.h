#ifndef ORESLANG_TASK_BOUND_STATE_REGION_H
#define ORESLANG_TASK_BOUND_STATE_REGION_H

/*
 * Trusted native state-region admission.
 *
 * This binds every bounded, non-parking OresStdSyncAdmission ticket to the
 * exact logical execution identity that requested it. A different task,
 * stale execution-store generation, or foreign region may not start, end,
 * release, cancel, or suspend through that ticket.
 *
 * The structure is fixed-capacity and heap-free. Guest code must never call
 * this API directly or construct identities/tickets; the scheduler supplies
 * authenticated OresNativeExecutionId + generation-checked store handles.
 */
#include "oreslang/runtime/NativeExecutionContext.h"
#include "oreslang/runtime/OresStdSyncAdmission.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct ores_state_task_identity {
    OresNativeExecutionId logical;
    OresNativeExecutionHandle handle;
} ores_state_task_identity;

typedef struct ores_state_region_binding {
    bool occupied;
    ores_std_ticket ticket;
    ores_state_task_identity owner;
} ores_state_region_binding;

typedef struct ores_task_bound_state_region {
    uint64_t region_id;
    uint64_t generation;
    atomic_flag metadata;
    ores_std_sync_gate gate;
    ores_state_region_binding bindings[ORES_STD_SYNC_CAPACITY];
} ores_task_bound_state_region;

#define ORES_TASK_BOUND_STATE_REGION_INIT(REGION_ID, GENERATION) { \
    .region_id=(REGION_ID), .generation=(GENERATION), .metadata=ATOMIC_FLAG_INIT, \
    .gate=ORES_STD_RW_INIT_SCOPE(GENERATION) }

static inline bool ores_state_task_identity_valid(ores_state_task_identity owner) {
    return owner.logical.runtime_domain != 0 && owner.logical.execution != 0 &&
           ores_native_store_handle_valid(owner.handle);
}

static inline bool ores_state_task_identity_equal(
        ores_state_task_identity left, ores_state_task_identity right) {
    return left.logical.runtime_domain == right.logical.runtime_domain &&
           left.logical.execution == right.logical.execution &&
           left.handle.slot == right.handle.slot &&
           left.handle.generation == right.handle.generation;
}

static inline bool ores_state_region_enter_metadata(
        ores_task_bound_state_region* region) {
    return region &&
        !atomic_flag_test_and_set_explicit(&region->metadata, memory_order_acquire);
}

static inline void ores_state_region_leave_metadata(
        ores_task_bound_state_region* region) {
    atomic_flag_clear_explicit(&region->metadata, memory_order_release);
}

static inline bool ores_state_region_ticket_equal(
        ores_std_ticket left, ores_std_ticket right) {
    return left.scope == right.scope &&
           left.scope_generation == right.scope_generation &&
           left.sequence == right.sequence;
}

static inline ores_state_region_binding* ores_state_region_find_binding(
        ores_task_bound_state_region* region, ores_std_ticket ticket) {
    if (!region || !ticket.sequence || ticket.scope != &region->gate ||
            ticket.scope_generation != region->generation)
        return NULL;
    for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; ++i) {
        ores_state_region_binding* binding = &region->bindings[i];
        if (binding->occupied &&
                ores_state_region_ticket_equal(binding->ticket, ticket))
            return binding;
    }
    return NULL;
}

static inline ores_state_region_binding* ores_state_region_free_binding(
        ores_task_bound_state_region* region) {
    for (unsigned int i = 0; i < ORES_STD_SYNC_CAPACITY; ++i)
        if (!region->bindings[i].occupied) return &region->bindings[i];
    return NULL;
}

static inline void ores_state_region_clear_binding(
        ores_state_region_binding* binding) {
    if (!binding) return;
    binding->occupied = false;
    binding->ticket = (ores_std_ticket){0};
    binding->owner = (ores_state_task_identity){0};
}

static inline bool ores_state_region_valid(
        const ores_task_bound_state_region* region) {
    return region && region->region_id != 0 && region->generation != 0 &&
           region->gate.scope_generation == region->generation &&
           region->gate.kind == ORES_STD_RW;
}

/* Request never parks a worker. GRANTED/QUEUED both carry a task-bound ticket. */
static inline ores_std_sync_result ores_state_region_request(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_access access) {
    if (!ores_state_region_valid(region) ||
            !ores_state_task_identity_valid(owner) ||
            (access != ORES_STD_READ && access != ORES_STD_WRITE))
        return ores_std_empty(ORES_STD_SYNC_INVALID);
    if (!ores_state_region_enter_metadata(region))
        return ores_std_empty(ORES_STD_SYNC_RETRY);

    ores_std_sync_result out = ores_std_request(&region->gate, access);
    if (out.code == ORES_STD_SYNC_GRANTED || out.code == ORES_STD_SYNC_QUEUED) {
        ores_state_region_binding* binding = ores_state_region_free_binding(region);
        /*
         * Both tables have identical capacity and a binding is retired only
         * after the corresponding OresStd ticket retires. If this invariant
         * fails, continuing could publish an unowned capability.
         */
        if (!binding) abort();
        binding->occupied = true;
        binding->ticket = out.ticket;
        binding->owner = owner;
    }
    ores_state_region_leave_metadata(region);
    return out;
}

static inline ores_std_sync_code ores_state_region_start(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_ticket ticket) {
    if (!ores_state_region_valid(region) ||
            !ores_state_task_identity_valid(owner))
        return ORES_STD_SYNC_INVALID;
    if (!ores_state_region_enter_metadata(region)) return ORES_STD_SYNC_RETRY;
    ores_state_region_binding* binding = ores_state_region_find_binding(region, ticket);
    ores_std_sync_code out = !binding
        ? ORES_STD_SYNC_STALE
        : !ores_state_task_identity_equal(binding->owner, owner)
            ? ORES_STD_SYNC_INVALID
            : ores_std_start(&region->gate, ticket);
    ores_state_region_leave_metadata(region);
    return out;
}

static inline ores_std_sync_code ores_state_region_end(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_ticket ticket) {
    if (!ores_state_region_valid(region) ||
            !ores_state_task_identity_valid(owner))
        return ORES_STD_SYNC_INVALID;
    if (!ores_state_region_enter_metadata(region)) return ORES_STD_SYNC_RETRY;
    ores_state_region_binding* binding = ores_state_region_find_binding(region, ticket);
    ores_std_sync_code out = !binding
        ? ORES_STD_SYNC_STALE
        : !ores_state_task_identity_equal(binding->owner, owner)
            ? ORES_STD_SYNC_INVALID
            : ores_std_end(&region->gate, ticket);
    ores_state_region_leave_metadata(region);
    return out;
}

/* Release doubles as queued cancellation, but only the ticket owner may do it. */
static inline ores_std_sync_result ores_state_region_release(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_ticket ticket) {
    if (!ores_state_region_valid(region) ||
            !ores_state_task_identity_valid(owner))
        return ores_std_empty(ORES_STD_SYNC_INVALID);
    if (!ores_state_region_enter_metadata(region))
        return ores_std_empty(ORES_STD_SYNC_RETRY);
    ores_state_region_binding* binding = ores_state_region_find_binding(region, ticket);
    ores_std_sync_result out;
    if (!binding) {
        out = ores_std_empty(ORES_STD_SYNC_STALE);
    } else if (!ores_state_task_identity_equal(binding->owner, owner)) {
        out = ores_std_empty(ORES_STD_SYNC_INVALID);
    } else {
        out = ores_std_release(&region->gate, ticket);
        if (out.code == ORES_STD_SYNC_GRANTED)
            ores_state_region_clear_binding(binding);
    }
    ores_state_region_leave_metadata(region);
    return out;
}

static inline ores_std_sync_code ores_state_region_ready_to_suspend(
        ores_task_bound_state_region* region,
        ores_state_task_identity owner,
        ores_std_ticket ticket) {
    if (!ores_state_region_valid(region) ||
            !ores_state_task_identity_valid(owner))
        return ORES_STD_SYNC_INVALID;
    if (!ores_state_region_enter_metadata(region)) return ORES_STD_SYNC_RETRY;
    ores_state_region_binding* binding = ores_state_region_find_binding(region, ticket);
    ores_std_sync_code out = !binding
        ? ORES_STD_SYNC_STALE
        : !ores_state_task_identity_equal(binding->owner, owner)
            ? ORES_STD_SYNC_INVALID
            : ores_std_ready_to_suspend(&region->gate, ticket);
    ores_state_region_leave_metadata(region);
    return out;
}

/* Supervisor stop: revoke queued/dormant grants, retain physically running loans. */
static inline ores_std_sync_result ores_state_region_seal(
        ores_task_bound_state_region* region) {
    if (!ores_state_region_valid(region))
        return ores_std_empty(ORES_STD_SYNC_INVALID);
    if (!ores_state_region_enter_metadata(region))
        return ores_std_empty(ORES_STD_SYNC_RETRY);
    ores_std_sync_result out = ores_std_seal(&region->gate);
    if (out.code == ORES_STD_SYNC_GRANTED || out.code == ORES_STD_SYNC_CLOSED) {
        for (unsigned int i = 0; i < out.failed_count; ++i) {
            ores_state_region_binding* binding =
                ores_state_region_find_binding(region, out.failed[i]);
            ores_state_region_clear_binding(binding);
        }
    }
    ores_state_region_leave_metadata(region);
    return out;
}

static inline ores_std_sync_code ores_state_region_quiescent(
        ores_task_bound_state_region* region) {
    if (!ores_state_region_valid(region)) return ORES_STD_SYNC_INVALID;
    if (!ores_state_region_enter_metadata(region)) return ORES_STD_SYNC_RETRY;
    ores_std_sync_code out = ores_std_quiescent(&region->gate);
    ores_state_region_leave_metadata(region);
    return out;
}

#endif
