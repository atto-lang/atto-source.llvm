#pragma once
#include "NativeF32ActorHostAdmissionV1.h"
#include <limits.h>
#include <pthread.h>
#include <stdint.h>

/* Host-private, single-physical-carrier f32 call lifetime pin. The scheduler
 * must call STOP + await quiescence before releasing the actor's memory.
 * The pin is not a guest capability, compiler lowering, or a tensor pointer.
 * Lock order for admission: native execution authority -> this lease mutex.
 * A borrowed OresF32ActorHostScope/context must remain immutable and alive.
 */
typedef struct {
    OresNativeExecutionAuthority *authority;
    pthread_mutex_t mutex;
    pthread_cond_t idle;
    pthread_t owner_thread;
    uint64_t serial;
    uint64_t active_serial;
    unsigned active;
    int stopped;
} OresF32ActorTurnLeaseV1;

typedef struct {
    OresF32ActorTurnLeaseV1 *lease;
    uint64_t serial;
    pthread_t owner_thread;
} OresF32ActorOperationPinV1;

static inline int ores_f32_actor_lease_init_v1(
        OresF32ActorTurnLeaseV1 *lease,
        OresNativeExecutionAuthority *authority) {
    if (!lease || !authority) return 0;
    lease->authority = authority;
    lease->serial = 0;
    lease->active_serial = 0;
    lease->active = 0;
    lease->stopped = 0;
    if (pthread_mutex_init(&lease->mutex, NULL) != 0) return 0;
    if (pthread_cond_init(&lease->idle, NULL) != 0) {
        (void)pthread_mutex_destroy(&lease->mutex);
        return 0;
    }
    return 1;
}

/* Atomically re-checks the physical RUNNING owner and increments the pin
 * under authority->mutex then lease->mutex. STOP uses the authority mutex,
 * so it cannot linearize between this final check and pin publication.
 * This closes the old point-in-time preflight TOCTOU window. */
static inline OresF32HostAdmissionResult ores_f32_actor_lease_enter_v1(
        OresF32ActorTurnLeaseV1 *lease,
        const OresF32ActorHostScope *scope,
        size_t lhs_rows, size_t lhs_cols, size_t rhs_rows, size_t rhs_cols,
        size_t max_output_bytes, OresF32ActorOperationPinV1 *out) {
    if (!lease || !scope || !out ||
            scope->physical_authority != lease->authority)
        return ORES_F32_HOST_INVALID;
    OresF32HostAdmissionResult check = ores_f32_host_preflight_v1(
            scope, lhs_rows, lhs_cols, rhs_rows, rhs_cols, max_output_bytes);
    if (check != ORES_F32_HOST_PREFLIGHT_ONLY) return check;
    OresNativeExecutionAuthority *a = lease->authority;
    ores_execution_lock(a);
    if (pthread_mutex_lock(&lease->mutex) != 0) __builtin_trap();
    const OresExecutionClaim claim = scope->physical_claim;
    const int owns = a->phase == ORES_EXEC_RUNNING &&
        claim.carrier && a->owner == claim.carrier &&
        claim.generation == a->generation &&
        claim.sequence == a->sequence &&
        pthread_equal(a->owner_thread, pthread_self());
    OresF32HostAdmissionResult decision = ORES_F32_HOST_NOT_RUNNING;
    if (owns && !lease->stopped && lease->active == 0 && lease->serial < UINT64_MAX) {
        lease->owner_thread = pthread_self();
        lease->active = 1;
        lease->active_serial = ++lease->serial;
        *out = (OresF32ActorOperationPinV1){lease, lease->active_serial, lease->owner_thread};
        decision = ORES_F32_HOST_PREFLIGHT_ONLY;
    }
    if (pthread_mutex_unlock(&lease->mutex) != 0) __builtin_trap();
    ores_execution_unlock(a);
    return decision;
}

/* Copied pins, stale serials, double returns and foreign threads cannot
 * decrement the live count or wake an early teardown. */
static inline int ores_f32_actor_lease_leave_v1(OresF32ActorOperationPinV1 pin) {
    OresF32ActorTurnLeaseV1 *lease = pin.lease;
    if (!lease || !pthread_equal(pin.owner_thread, pthread_self())) return 0;
    if (pthread_mutex_lock(&lease->mutex) != 0) __builtin_trap();
    const int ok = lease->active == 1 && pin.serial != 0 &&
        pin.serial == lease->active_serial &&
        pthread_equal(lease->owner_thread, pthread_self());
    if (ok) {
        lease->active = 0;
        lease->active_serial = 0;
        if (pthread_cond_broadcast(&lease->idle) != 0) __builtin_trap();
    }
    if (pthread_mutex_unlock(&lease->mutex) != 0) __builtin_trap();
    return ok;
}

/* Called by supervisor. STOP is linearized by NativeExecutionAuthority.
 * A failed stop does not reopen the gate; external STOPPING is fail-closed. */
static inline int ores_f32_actor_lease_stop_v1(
        OresF32ActorTurnLeaseV1 *lease, uint64_t generation) {
    if (!lease) return 0;
    const int stopped_authority = ores_execution_stop(lease->authority, generation);
    if (pthread_mutex_lock(&lease->mutex) != 0) __builtin_trap();
    lease->stopped = 1;
    if (pthread_cond_broadcast(&lease->idle) != 0) __builtin_trap();
    if (pthread_mutex_unlock(&lease->mutex) != 0) __builtin_trap();
    return stopped_authority;
}

/* Only a supervisor thread may wait for quiescence; the owner must release
 * its pin first. Waiters and entrants must be externally joined/fenced
 * before destroying the lease or native authority. */
static inline int ores_f32_actor_lease_wait_quiescent_v1(
        OresF32ActorTurnLeaseV1 *lease) {
    if (!lease) return 0;
    if (pthread_mutex_lock(&lease->mutex) != 0) __builtin_trap();
    if (lease->active && pthread_equal(lease->owner_thread, pthread_self())) {
        (void)pthread_mutex_unlock(&lease->mutex);
        return 0;
    }
    while (!lease->stopped || lease->active) {
        if (pthread_cond_wait(&lease->idle, &lease->mutex) != 0) __builtin_trap();
    }
    if (pthread_mutex_unlock(&lease->mutex) != 0) __builtin_trap();
    return 1;
}

static inline int ores_f32_actor_lease_destroy_v1(
        OresF32ActorTurnLeaseV1 *lease) {
    if (!lease) return 0;
    if (pthread_mutex_lock(&lease->mutex) != 0) __builtin_trap();
    const int safe = lease->stopped && lease->active == 0;
    if (pthread_mutex_unlock(&lease->mutex) != 0) __builtin_trap();
    if (!safe) return 0;
    if (pthread_cond_destroy(&lease->idle) != 0) return 0;
    return pthread_mutex_destroy(&lease->mutex) == 0;
}
