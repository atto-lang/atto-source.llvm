#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeExecutionAuthority.h"
#include "oreslang/runtime/NativeExecutionContext.h"
#include "oreslang/runtime/NativePhysicalTurnGate.h"
#include "oreslang/runtime/NativeSelectArbiter.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>
#include <unistd.h>
#ifdef ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE
#include <errno.h>
/* Only compiled in the isolated fault-injection CTest; absent in production. */
static _Atomic int fail_next_carrier_create;
void ores_native_actor_scheduler_test_fail_next_create(void) {
    atomic_store_explicit(&fail_next_carrier_create, 1, memory_order_relaxed);
}
#endif
static int create_carrier(pthread_t* tid, void* context);

typedef enum {
    WAKE_EMPTY = 0, WAKE_RESERVED = 1, WAKE_TRANSFERRED = 2,
    WAKE_READY = 3, WAKE_EXECUTING = 4
} WakeState;
typedef struct {
    OresNativeWakeTicket ticket;
    OresNativeWakeOutcome outcome;
    WakeState state;
    uint64_t generation; /* never recycled at UINT64_MAX */
    OresNativeSelectArbiter* select_arbiter;
    uint64_t select_generation;
    unsigned select_bound;
} WakeCredit;
typedef struct {
    uint64_t generation; /* descriptor generation, never recycled at UINT64_MAX */
    uint64_t sequence;   /* target-local FIFO order */
    uint64_t payload;
    OresNativeAsyncSendState state;
    OresNativeSendStatus outcome;
    OresNativeWakeTicket wake;
    unsigned wake_bound;
    OresNativeSelectToken select_token;
    unsigned select_bound;
    /* A terminal descriptor cannot be physically retired while its bound
     * wake callback still owns WAKE_EXECUTING. The external release call may
     * claim one-shot retirement; the carrier completes it after callback exit. */
    unsigned release_pending;
} AsyncSendCredit;
typedef struct {
    uint64_t* messages; /* fixed ring, assigned at scheduler startup */
    size_t head, tail, queued;
    unsigned active;
    pthread_t carrier_owner; /* owning physical worker while active */
    unsigned carrier_owner_valid;
    unsigned has_control;
    WakeCredit* wakes; /* preallocated producer-local escrow credits */
    size_t wake_live, wake_ready, wake_cursor;
    AsyncSendCredit* async_sends; /* preallocated target-local Future descriptors */
    size_t async_live, async_pending;
    uint64_t next_async_sequence;
    uint64_t control_payload;
    OresNativeActorTerminalFailure failure;
    unsigned control_is_failure;
    unsigned spawned, retired;
    uint64_t generation;
    uint64_t accepted_user_messages;
    uint64_t processed_user_messages;
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
    size_t test_waiting_sends;
#endif
    uint64_t abandoned_user_messages;
    OresNativeActorTerminationKind requested_terminal_kind;
    unsigned terminal_valid;
    OresNativeActorTerminalState terminal;
    /* Stable actor-lifetime allocation capability, independent of turn ids. */
    OresNativeAllocationDomainRef allocation_domain;
    OresNativeExecutionAuthority authority;
} ActorLane;
struct OresNativeActorScheduler {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t drained;
    pthread_cond_t credit;
    ActorLane* actors;
    uint64_t* slots;
    WakeCredit* wake_slots;
    AsyncSendCredit* async_send_slots;
    pthread_t* threads;
    size_t actor_count, per_actor_capacity, worker_count, started_workers;
    size_t next_actor, outstanding, runnable_lanes;
    uint64_t domain;
    uint64_t next_execution;
    uint64_t next_select_generation;
    uint64_t reserved_executions;
    uint64_t reserved_wakes; /* future accepted/execution credits */
    uint64_t accepted, rejected, completed, control_completed, wakes_completed;
    int retired;
    int priority_inheritance_enabled;
    int explicit_carrier_policy;
    int carrier_policy;
    int carrier_priority;
    OresNativeActorTaskFn callback;
    OresNativeActorTaskFn control_callback;
    OresNativeActorPanicFn panic_callback;
    OresNativeActorWakeFn wake_callback;
    void* context;
    OresNativePhysicalTurnGate* physical_turn_gate;
};
static _Thread_local OresNativeActorScheduler* executing;
static _Thread_local OresNativeActorRef executing_actor;
static _Thread_local OresExecutionClaim executing_claim;
static _Thread_local OresNativeExecutionContext executing_context;
static _Thread_local OresNativeWakeTicket executing_wake;
static _Thread_local int executing_wake_valid;
static _Thread_local int carrier_identity;
static _Atomic(uint64_t) next_scheduler_domain = 1;

/* Never wrap a scheduler-domain capability back to 1: a stale ActorRef
 * must not match a new scheduler even after counter exhaustion. The
 * UINT64_MAX value is permanently reserved as the exhausted sentinel.
 * A CAS loop also preserves uniqueness under concurrent scheduler opens. */
static uint64_t allocate_scheduler_domain(void) {
    uint64_t candidate = atomic_load_explicit(
        &next_scheduler_domain, memory_order_relaxed);
    while (candidate != 0 && candidate != UINT64_MAX) {
        if (atomic_compare_exchange_weak_explicit(
                &next_scheduler_domain, &candidate, candidate + 1,
                memory_order_relaxed, memory_order_relaxed))
            return candidate;
    }
    return 0;
}
#ifdef ORESLANG_NATIVE_TEST_DOMAIN_EXHAUSTION
/* Only compiled in the isolated security test binary, never production. */
void ores_native_actor_scheduler_test_set_next_domain(uint64_t value) {
    atomic_store_explicit(&next_scheduler_domain, value, memory_order_relaxed);
}
#endif

static int init_scheduler_mutex(OresNativeActorScheduler* s) {
    pthread_mutexattr_t attr;
    int attr_ready = pthread_mutexattr_init(&attr) == 0;
    if (attr_ready) {
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT >= 0
        if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0
                && pthread_mutex_init(&s->lock, &attr) == 0) {
            s->priority_inheritance_enabled = 1;
            if (pthread_mutexattr_destroy(&attr) != 0) abort();
            return 1;
        }
#endif
        if (pthread_mutexattr_destroy(&attr) != 0) abort();
    }
    /* General-purpose execution remains portable when PI is unavailable.
     * Hard-RT admission must separately require the query below to be true. */
    s->priority_inheritance_enabled = 0;
    return pthread_mutex_init(&s->lock, NULL) == 0;
}
static void lock(OresNativeActorScheduler* s) {
    if (pthread_mutex_lock(&s->lock) != 0) abort();
}
static void unlock(OresNativeActorScheduler* s) {
    if (pthread_mutex_unlock(&s->lock) != 0) abort();
}
static void publish_terminal_locked(
        OresNativeActorScheduler* s, size_t actor_index) {
    ActorLane* lane = &s->actors[actor_index];
    if (!lane->spawned || lane->terminal_valid
            || !lane->retired || lane->active || lane->queued || lane->has_control
            || lane->wake_live || lane->wake_ready)
        return;
    if (ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED)
        return;
    if (lane->requested_terminal_kind != ORES_NATIVE_TERMINATION_STOPPED
            && lane->requested_terminal_kind != ORES_NATIVE_TERMINATION_RUNTIME_RETIRED)
        abort();
    if (lane->processed_user_messages > lane->accepted_user_messages)
        abort();
    if (lane->abandoned_user_messages
            > lane->accepted_user_messages - lane->processed_user_messages)
        abort();
    if (lane->processed_user_messages + lane->abandoned_user_messages
            != lane->accepted_user_messages)
        abort();
    lane->terminal = (OresNativeActorTerminalState){
        .actor = {s->domain, (uint64_t)actor_index, lane->generation},
        .kind = lane->requested_terminal_kind,
        .accepted_user_messages = lane->accepted_user_messages,
        .processed_user_messages = lane->processed_user_messages,
        .abandoned_user_messages = lane->abandoned_user_messages,
        .abandoned_control_messages = 0,
    };
    lane->terminal_valid = 1;
}
#ifdef ORESLANG_NATIVE_TEST_EXECUTION_EXHAUSTION
int ores_native_actor_scheduler_test_set_next_execution(
        OresNativeActorScheduler* s, uint64_t next_execution) {
    if (!s || !next_execution) return 0;
    lock(s);
    const int ok = !s->retired && s->outstanding == 0
        && s->reserved_executions == 0 && s->reserved_wakes == 0;
    if (ok) s->next_execution = next_execution;
    unlock(s);
    return ok;
}
#endif
/* Supervisor CONTROL outranks WAKE which outranks DATA. */
static int find_ready(OresNativeActorScheduler* s, size_t* actor, int* kind,
        size_t recent_actor, unsigned consecutive) {
    for (int pass = 2; pass >= 1; --pass) {
        for (size_t offset = 0; offset < s->actor_count; ++offset) {
            size_t i = (s->next_actor + offset) % s->actor_count;
            ActorLane* lane = &s->actors[i];
            if (!!lane->active != !!lane->carrier_owner_valid) abort();
            if (!lane->active
                    && (pass == 2 ? lane->has_control : lane->wake_ready)) {
                *actor = i;
                *kind = pass;
                s->next_actor = (i + 1) % s->actor_count;
                return 1;
            }
        }
    }
    if (recent_actor < s->actor_count && consecutive < 2
            && !s->actors[recent_actor].active
            && s->actors[recent_actor].queued) {
        *actor = recent_actor; *kind = 0;
        s->next_actor = (recent_actor + 1) % s->actor_count;
        return 1;
    }
    for (size_t offset = 0; offset < s->actor_count; ++offset) {
        size_t i = (s->next_actor + offset) % s->actor_count;
        if (!s->actors[i].active && s->actors[i].queued) {
            *actor = i; *kind = 0;
            s->next_actor = (i + 1) % s->actor_count;
            return 1;
        }
    }
    return 0;
}
static int lane_has_work(const ActorLane* lane) {
    return lane->queued || lane->has_control || lane->wake_ready;
}
static void maybe_finish_stopped_locked(OresNativeActorScheduler* s, size_t i) {
    ActorLane* lane = &s->actors[i];
    if (!lane->retired || lane->active || lane_has_work(lane) || lane->wake_live)
        return;
    if (ores_execution_phase(&lane->authority) == ORES_EXEC_STOPPING
            && !ores_execution_retired(&lane->authority, lane->generation))
        abort();
    publish_terminal_locked(s, i);
    pthread_cond_broadcast(&s->drained);
}
static void release_wake_locked(OresNativeActorScheduler* s,
        size_t i, WakeCredit* credit) {
    ActorLane* lane = &s->actors[i];
    if (!lane->wake_live || !credit || (credit->state != WAKE_RESERVED
            && credit->state != WAKE_TRANSFERRED) || !s->reserved_wakes)
        abort();
    --s->reserved_wakes;
    --lane->wake_live;
    credit->state = WAKE_EMPTY;
    credit->ticket = (OresNativeWakeTicket){0};
    credit->select_arbiter = NULL;
    credit->select_generation = 0;
    credit->select_bound = 0;
    maybe_finish_stopped_locked(s, i);
}
static WakeCredit* matching_wake_locked(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket);
static AsyncSendCredit* matching_async_send_locked(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket);
static void retire_async_send_credit_locked(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket,
        AsyncSendCredit* credit);
static void settle_async_send_wake_locked(
        OresNativeActorScheduler* s, AsyncSendCredit* credit);
static OresNativeWakePublishStatus publish_select_wake_locked(
        OresNativeActorScheduler* s,
        OresNativeSelectToken token,
        OresNativeWakeOutcome outcome);
static void service_async_sends_locked(
        OresNativeActorScheduler* s, size_t actor_index);
static void fail_pending_async_sends_locked(
        OresNativeActorScheduler* s, size_t actor_index,
        OresNativeSendStatus outcome);
static void* carrier(void* raw) {
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)raw;
    size_t recent_actor = SIZE_MAX;
    unsigned consecutive = 0;
    for (;;) {
        lock(s);
        size_t i = 0;
        int kind = 0;
        int gate_held = 0;
        for (;;) {
            int found = find_ready(s, &i, &kind, recent_actor, consecutive);
            if (found && (!s->physical_turn_gate
                    || ores_native_turn_gate_try_acquire(s->physical_turn_gate))) {
                gate_held = s->physical_turn_gate != NULL;
                break;
            }
            /* No actor turn begins when the process-wide permit is full.
             * Remain idle on the scheduler condition, not in a guest await
             * or blocking lock. The releasing domain broadcasts wakeups. */
            if (s->retired && !s->outstanding) {
                unlock(s);
                return NULL;
            }
            /* Register before the final permit recheck. A release which saw
             * zero waiters cannot race us into sleeping with a free token;
             * a later release will acquire this scheduler mutex only after
             * cond_wait has atomically released it. */
            OresNativePhysicalTurnGate* gate = s->physical_turn_gate;
            const int waiting_on_gate = found && gate != NULL;
            if (waiting_on_gate) {
                atomic_fetch_add_explicit(&gate->waiting, 1, memory_order_acq_rel);
                if (ores_native_turn_gate_try_acquire(gate)) {
                    atomic_fetch_sub_explicit(&gate->waiting, 1, memory_order_acq_rel);
                    gate_held = 1;
                    break;
                }
            }
            if (pthread_cond_wait(&s->ready, &s->lock) != 0) abort();
            if (waiting_on_gate) {
                atomic_fetch_sub_explicit(&gate->waiting, 1, memory_order_acq_rel);
            }
        }
        consecutive = (i == recent_actor && kind == 0)
                ? (consecutive < 2 ? consecutive + 1 : 2) : 1;
        recent_actor = i;
        ActorLane* lane = &s->actors[i];
        uint64_t payload = 0;
        OresNativeActorTerminalFailure failure = {0};
        OresNativeWakeTicket wake_ticket = {0};
        OresNativeWakeOutcome wake_outcome = ORES_NATIVE_WAKE_FAILED;
        WakeCredit* active_wake = NULL;
        int typed_failure = kind == 2 && lane->control_is_failure;
        if (kind == 2) {
            payload = lane->control_payload;
            if (typed_failure) failure = lane->failure;
            lane->control_is_failure = 0;
            lane->has_control = 0;
        } else if (kind == 1) {
            for (size_t offset = 0; offset < ORES_NATIVE_WAKE_CREDITS_PER_ACTOR; ++offset) {
                size_t w = (lane->wake_cursor + offset)
                    % ORES_NATIVE_WAKE_CREDITS_PER_ACTOR;
                if (lane->wakes[w].state == WAKE_READY) {
                    active_wake = &lane->wakes[w];
                    lane->wake_cursor = (w + 1)
                        % ORES_NATIVE_WAKE_CREDITS_PER_ACTOR;
                    break;
                }
            }
            if (!active_wake || !lane->wake_ready) abort();
            wake_ticket = active_wake->ticket;
            wake_outcome = active_wake->outcome;
            active_wake->state = WAKE_EXECUTING;
            --lane->wake_ready;
        } else {
            payload = lane->messages[lane->head];
            lane->head = (lane->head + 1) % s->per_actor_capacity;
            lane->queued--;
        }
        if (lane->active || lane->carrier_owner_valid) abort();
        if (!s->reserved_executions || !s->next_execution) abort();
        --s->reserved_executions;
        const uint64_t execution_id = s->next_execution;
        s->next_execution = execution_id == UINT64_MAX ? 0 : execution_id + 1;
        OresExecutionClaim execution_claim = {0};
        /* Only this scheduler can certify that the dequeued envelope was
         * admitted before retirement. Never time out an active guest owner. */
        if (!ores_execution_enter(&lane->authority, lane->generation,
                (uint64_t)(uintptr_t)&carrier_identity, 1, &execution_claim))
            abort();
        lane->active = 1;
        lane->carrier_owner = pthread_self();
        lane->carrier_owner_valid = 1;
        unlock(s);

        // Actor identity is authorized only by the physical execution claim.
        executing_actor = (OresNativeActorRef){s->domain, (uint64_t)i, lane->generation};
        executing_claim = execution_claim;
        executing_context = (OresNativeExecutionContext){
            .id = {s->domain, execution_id},
            .allocation = lane->allocation_domain,
            .parent_execution = {0, 0},
            .continuation = {0, 0}
        };
        executing = s;
        executing_wake_valid = kind == 1;
        executing_wake = wake_ticket;
        if (typed_failure) s->panic_callback(s->context, i, failure);
        else if (kind == 2) s->control_callback(s->context, i, payload);
        else if (kind == 1) s->wake_callback(s->context, i, wake_ticket, wake_outcome);
        else s->callback(s->context, i, payload);
        executing_wake_valid = 0;
        executing_wake = (OresNativeWakeTicket){0};
        executing_actor = (OresNativeActorRef){0, 0, 0};
        executing_claim = (OresExecutionClaim){0};
        executing_context = (OresNativeExecutionContext){0};
        executing = NULL;

        lock(s);
        if (!lane->active || !lane->carrier_owner_valid
            || !pthread_equal(lane->carrier_owner, pthread_self())
            || !s->outstanding || s->completed == UINT64_MAX) abort();
        /* Leave the generation-checked guest execution claim first, but
         * keep lane.active/outstanding until the gate has completed its
         * cross-domain wakeup. Otherwise drain() may report quiescence and
         * the VM may destroy a scheduler while a gate-release callback is
         * still trying to signal that scheduler. */
        if (!ores_execution_leave(&lane->authority, execution_claim, 0)) abort();
        unlock(s);
        if (gate_held) ores_native_turn_gate_release(s->physical_turn_gate);
        lock(s);
        if (!lane->active || !lane->carrier_owner_valid
            || !pthread_equal(lane->carrier_owner, pthread_self())
            || !s->outstanding || s->completed == UINT64_MAX) abort();
        lane->active = 0;
        lane->carrier_owner_valid = 0;

        /*
         * The finishing active turn itself kept this lane in runnable_lanes.
         * If no queued/CONTROL/WAKE work remains, retire that count BEFORE
         * servicing a pending async-send Future. enqueue_locked() can then
         * re-add the lane exactly once when it publishes the transferred DATA
         * envelope. This is also the no-slot-steal linearization boundary.
         */
        if (!lane_has_work(lane)) {
            if (!s->runnable_lanes) abort();
            --s->runnable_lanes;
        }

        service_async_sends_locked(s, i);

        /*
         * try_send charges lane->active against USER mailbox capacity.
         * Future waiters/status observers share this condition, so publish
         * only after pending async descriptors have received first refusal.
         */
        if (pthread_cond_broadcast(&s->credit) != 0) abort();
        s->outstanding--;
        s->completed++;
        if (kind == 0) {
            if (lane->processed_user_messages == UINT64_MAX) abort();
            ++lane->processed_user_messages;
        } else if (kind == 1) {
            if (!active_wake || active_wake->state != WAKE_EXECUTING
                    || !lane->wake_live || s->wakes_completed == UINT64_MAX)
                abort();

            /* A descriptor release requested by the trusted observer while
             * this callback was executing is only a logical one-shot claim.
             * Retire the descriptor after callback return, while both wake
             * and descriptor identities are still generation-checkable. */
            AsyncSendCredit* deferred_release = NULL;
            OresNativeAsyncSendTicket deferred_ticket = {0};
            if (wake_ticket.descriptor_slot != UINT64_MAX) {
                deferred_ticket = (OresNativeAsyncSendTicket){
                    .target = wake_ticket.target,
                    .slot = wake_ticket.descriptor_slot,
                    .generation = wake_ticket.descriptor_generation
                };
                deferred_release =
                    matching_async_send_locked(s, deferred_ticket);
                if (deferred_release && deferred_release->release_pending) {
                    WakeCredit* descriptor_wake =
                        deferred_release->wake_bound
                            ? matching_wake_locked(s, deferred_release->wake)
                            : NULL;
                    if (descriptor_wake != active_wake) abort();
                } else {
                    deferred_release = NULL;
                }
            }

            active_wake->state = WAKE_EMPTY;
            active_wake->ticket = (OresNativeWakeTicket){0};
            active_wake->select_arbiter = NULL;
            active_wake->select_generation = 0;
            active_wake->select_bound = 0;
            --lane->wake_live;
            ++s->wakes_completed;
            if (deferred_release)
                retire_async_send_credit_locked(
                    s, deferred_ticket, deferred_release);
        }
        if (kind == 2) ++s->control_completed;
        if (lane_has_work(lane)) pthread_cond_signal(&s->ready);
        maybe_finish_stopped_locked(s, i);
        if (!s->outstanding) {
            pthread_cond_broadcast(&s->drained);
            if (s->retired) pthread_cond_broadcast(&s->ready);
        }
        unlock(s);
    }
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_with_wakes(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    OresNativeActorPanicFn panic_callback, OresNativeActorWakeFn wake_callback,
    void* ctx) {
    if (!callback || !actors || actors > 1024 || !capacity || capacity > 65536
        || !workers || workers > 128 || actors > SIZE_MAX / capacity)
        return NULL;
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    size_t initialized = 0;
    s->actors = (ActorLane*)calloc(actors, sizeof(ActorLane));
    s->slots = (uint64_t*)calloc(actors * capacity, sizeof(uint64_t));
    /* Wake escrow is fixed-capacity runtime metadata. Preallocate it even for
     * legacy VM opens so a trusted host may activate cooperative continuation
     * wakes before admitting any work, without allocating on the async path. */
    s->wake_slots = (WakeCredit*)calloc(
        actors * ORES_NATIVE_WAKE_CREDITS_PER_ACTOR, sizeof(WakeCredit));
    s->async_send_slots = (AsyncSendCredit*)calloc(
        actors * ORES_NATIVE_ASYNC_SENDS_PER_ACTOR, sizeof(AsyncSendCredit));
    s->threads = (pthread_t*)calloc(workers, sizeof(pthread_t));
    if (!s->actors || !s->slots || !s->wake_slots
            || !s->async_send_slots || !s->threads)
        goto allocation_failed;
    s->domain = allocate_scheduler_domain();
    if (!s->domain) goto allocation_failed;
    s->actor_count = actors;
    s->next_execution = 1;
    s->next_select_generation = 1;
    s->per_actor_capacity = capacity;
    s->worker_count = workers;
    s->callback = callback;
    s->control_callback = control_callback;
    s->panic_callback = panic_callback;
    s->wake_callback = wake_callback;
    s->context = ctx;
    for (size_t i = 0; i < actors; ++i) {
        s->actors[i].messages = s->slots + i * capacity;
        s->actors[i].wakes =
            s->wake_slots + i * ORES_NATIVE_WAKE_CREDITS_PER_ACTOR;
        s->actors[i].async_sends = s->async_send_slots
            + i * ORES_NATIVE_ASYNC_SENDS_PER_ACTOR;
        s->actors[i].next_async_sequence = 1;
        if (!ores_execution_init(&s->actors[i].authority)) goto allocation_failed;
        ++initialized;
    }
    if (!init_scheduler_mutex(s)) goto allocation_failed;
    if (pthread_cond_init(&s->ready, NULL) != 0) {
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    if (pthread_cond_init(&s->drained, NULL) != 0) {
        pthread_cond_destroy(&s->ready);
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    if (pthread_cond_init(&s->credit, NULL) != 0) {
        pthread_cond_destroy(&s->drained);
        pthread_cond_destroy(&s->ready);
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    /* Metadata-only bootstrap. The first accepted message starts carriers. */
    return s;
allocation_failed:
    for (size_t i = 0; i < initialized; ++i)
        if (!ores_execution_destroy(&s->actors[i].authority)) abort();
    free(s->threads);
    free(s->slots);
    free(s->wake_slots);
    free(s->async_send_slots);
    free(s->actors);
    free(s);
    return NULL;
}
int ores_native_actor_scheduler_configure_wake_callback(
        OresNativeActorScheduler* s, OresNativeActorWakeFn wake_callback) {
    if (!s || !wake_callback) return 0;
    lock(s);
    int ok = !s->retired
        && s->wake_callback == NULL
        && s->started_workers == 0
        && s->accepted == 0
        && s->outstanding == 0
        && s->reserved_wakes == 0;
    if (ok) {
        for (size_t i = 0; i < s->actor_count; ++i) {
            const ActorLane* lane = &s->actors[i];
            if (lane->spawned || lane->retired || lane->wake_live
                    || lane->wake_ready) {
                ok = 0;
                break;
            }
        }
    }
    if (ok) s->wake_callback = wake_callback;
    unlock(s);
    return ok;
}

OresNativeActorScheduler* ores_native_actor_scheduler_open_typed_control(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    OresNativeActorPanicFn panic_callback, void* ctx) {
    return ores_native_actor_scheduler_open_with_wakes(
        actors, capacity, workers, callback, control_callback,
        panic_callback, NULL, ctx);
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_control(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    void* ctx) {
    return ores_native_actor_scheduler_open_typed_control(
        actors, capacity, workers, callback, control_callback, NULL, ctx);
}
OresNativeActorScheduler* ores_native_actor_scheduler_open(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, void* ctx) {
    return ores_native_actor_scheduler_open_control(
        actors, capacity, workers, callback, NULL, ctx);
}
static int matching(OresNativeActorScheduler* s, OresNativeActorRef ref) {
    return s && ref.scheduler_domain == s->domain
        && ref.actor_slot < s->actor_count
        && ref.generation != 0
        && s->actors[ref.actor_slot].spawned
        && s->actors[ref.actor_slot].generation == ref.generation;
}
/* The scheduler and its bounded queues exist at VM bootstrap, but no OS
 * carriers are created until runnable work is actually admitted. Creation is
 * under the scheduler mutex: competing producers cannot over-provision or
 * race destruction. Workers follow DISTINCT occupied actor lanes, not the
 * number of queued messages: one serialized actor cannot wake five idle
 * pthreads just by filling its own mailbox. Failed creation never
 * acknowledges an unserviceable first message. */
static int create_carrier(pthread_t* tid, void* context) {
#ifdef ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE
    /* Fail exactly the next OS worker creation, without ever linking this
     * fault injector into the production ABI or exported symbol table. */
    if (atomic_exchange_explicit(
            &fail_next_carrier_create, 0, memory_order_relaxed))
        return EAGAIN;
#endif
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)context;
    if (!s->explicit_carrier_policy)
        return pthread_create(tid, NULL, carrier, context);

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    if (rc == 0) rc = pthread_attr_setschedpolicy(&attr, s->carrier_policy);
    if (rc == 0) {
        struct sched_param param = {0};
        param.sched_priority = s->carrier_priority;
        rc = pthread_attr_setschedparam(&attr, &param);
    }
    if (rc == 0) rc = pthread_create(tid, &attr, carrier, context);
    if (pthread_attr_destroy(&attr) != 0) abort();
    return rc;
}
static int execution_slot_available_locked(OresNativeActorScheduler* s) {
    /* reserved_executions counts admitted turns that have not yet claimed an
     * execution id. Keep enough remaining ids for every reservation so
     * exhaustion is reported at admission rather than crashing a carrier. */
    return s->next_execution != 0
        && s->reserved_executions <= UINT64_MAX - s->next_execution
        && s->reserved_wakes <= UINT64_MAX - s->next_execution
                                   - s->reserved_executions;
}
static int ensure_workers_locked(OresNativeActorScheduler* s, size_t required) {
    if (required > s->worker_count) required = s->worker_count;
    while (s->started_workers < required) {
        if (create_carrier(&s->threads[s->started_workers], s) != 0)
            break;
        ++s->started_workers;
    }
    /* An existing busy carrier cannot be counted as additional capacity.
     * Never acknowledge another independent lane on a failed growth step. */
    return s->started_workers >= required;
}
int ores_native_actor_scheduler_configure_fifo(
        OresNativeActorScheduler* s, int priority) {
    if (!s || executing == s) return 0;
    int minimum = sched_get_priority_min(SCHED_FIFO);
    int maximum = sched_get_priority_max(SCHED_FIFO);
    if (minimum < 0 || maximum < 0 || priority < minimum || priority > maximum)
        return 0;
    lock(s);
    /* Configuration is bootstrap-only. Spawning actor identities is harmless,
     * but no OS carrier or admitted work may exist yet. */
    int pristine = !s->retired && s->started_workers == 0
        && s->accepted == 0 && s->outstanding == 0 && s->runnable_lanes == 0;
    if (pristine) {
        s->explicit_carrier_policy = 1;
        s->carrier_policy = SCHED_FIFO;
        s->carrier_priority = priority;
    }
    unlock(s);
    return pristine;
}
int ores_native_actor_scheduler_fifo_carriers_ready(
        OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    int ready = s->explicit_carrier_policy
        && s->carrier_policy == SCHED_FIFO
        && s->started_workers == s->worker_count;
    unlock(s);
    return ready;
}
int ores_native_actor_scheduler_prestart_carriers(
        OresNativeActorScheduler* s) {
    if (!s || executing == s) return 0;
    lock(s);
    /* This primitive is intentionally narrower than "prepare realtime".
     * It proves only that pthread creation has been moved before admission.
     * OS scheduling, affinity, memory residency and synchronization bounds
     * remain separate hard-RT qualification requirements. */
    int pristine = !s->retired && s->accepted == 0
        && s->outstanding == 0 && s->runnable_lanes == 0;
    int ok = pristine && ensure_workers_locked(s, s->worker_count);
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_prestart_realtime(
        OresNativeActorScheduler* s) {
    return ores_native_actor_scheduler_prestart_carriers(s);
}
int ores_native_actor_scheduler_all_carriers_started(
        OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    int ready = s->started_workers == s->worker_count;
    unlock(s);
    return ready;
}
int ores_native_actor_scheduler_priority_inheritance_enabled(
        OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    int enabled = s->priority_inheritance_enabled;
    for (size_t i = 0; enabled && i < s->actor_count; ++i)
        enabled = ores_execution_priority_inheritance_enabled(
            &s->actors[i].authority);
    unlock(s);
    return enabled;
}
static int enqueue_locked(OresNativeActorScheduler* s, size_t i, uint64_t payload) {
    if (s->retired || i >= s->actor_count
        || !s->actors[i].spawned || s->actors[i].retired
        || s->actors[i].queued + s->actors[i].active >= s->per_actor_capacity
        || s->accepted >= UINT64_MAX - s->reserved_wakes
        || !execution_slot_available_locked(s)) {
        if (s->rejected != UINT64_MAX) s->rejected++;
        return 0;
    }
    ActorLane* a = &s->actors[i];
    const int new_lane = !a->active && !lane_has_work(a);
    if (!ensure_workers_locked(s, s->runnable_lanes + (size_t)new_lane)) {
        if (s->rejected != UINT64_MAX) ++s->rejected;
        return 0;
    }
    if (new_lane) ++s->runnable_lanes;
    a->messages[a->tail] = payload;
    a->tail = (a->tail + 1) % s->per_actor_capacity;
    a->queued++;
    if (a->accepted_user_messages == UINT64_MAX) abort();
    ++a->accepted_user_messages;
    s->reserved_executions++;
    s->accepted++;
    s->outstanding++;
    pthread_cond_signal(&s->ready);
    return 1;
}

static AsyncSendCredit* matching_async_send_locked(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket) {
    if (!s || !matching(s, ticket.target)
            || ticket.slot >= ORES_NATIVE_ASYNC_SENDS_PER_ACTOR
            || !ticket.generation)
        return NULL;
    ActorLane* lane = &s->actors[ticket.target.actor_slot];
    AsyncSendCredit* credit = &lane->async_sends[ticket.slot];
    if (credit->state == ORES_NATIVE_ASYNC_SEND_INVALID
            || credit->generation != ticket.generation)
        return NULL;
    return credit;
}

static void retire_async_send_credit_locked(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket,
        AsyncSendCredit* credit) {
    if (!s || !credit || credit->state == ORES_NATIVE_ASYNC_SEND_PENDING
            || credit->state == ORES_NATIVE_ASYNC_SEND_INVALID)
        abort();
    ActorLane* lane = &s->actors[ticket.target.actor_slot];
    if (!lane->async_live) abort();
    --lane->async_live;
    credit->state = ORES_NATIVE_ASYNC_SEND_INVALID;
    credit->outcome = ORES_NATIVE_SEND_INVALID_REF;
    credit->sequence = 0;
    credit->payload = 0;
    credit->wake = (OresNativeWakeTicket){0};
    credit->wake_bound = 0;
    credit->select_token = (OresNativeSelectToken){0};
    credit->select_bound = 0;
    credit->release_pending = 0;
    if (pthread_cond_broadcast(&s->credit) != 0) abort();
}

static AsyncSendCredit* oldest_pending_async_locked(ActorLane* lane) {
    AsyncSendCredit* oldest = NULL;
    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        AsyncSendCredit* candidate = &lane->async_sends[i];
        if (candidate->state != ORES_NATIVE_ASYNC_SEND_PENDING)
            continue;
        if (!candidate->sequence) abort();
        if (!oldest || candidate->sequence < oldest->sequence)
            oldest = candidate;
    }
    return oldest;
}

static void settle_selectable_failure_locked(
        AsyncSendCredit* credit,
        OresNativeSendStatus outcome) {
    if (!credit || credit->state != ORES_NATIVE_ASYNC_SEND_PENDING)
        abort();
    if (!credit->select_bound) {
        credit->state = ORES_NATIVE_ASYNC_SEND_FAILED;
        credit->outcome = outcome;
        return;
    }

    /*
     * A terminal send failure is itself a selectable terminal event. It may
     * claim the common winner only if no other arm committed first. If this
     * arm already lost/sealed/staled, retire it as CANCELLED without running
     * a guest branch or fabricating target admission.
     */
    const OresNativeSelectCommitResult selected =
        ores_native_select_try_commit(credit->select_token);
    if (selected == ORES_NATIVE_SELECT_WON) {
        credit->state = ORES_NATIVE_ASYNC_SEND_FAILED;
        credit->outcome = outcome;
    } else {
        credit->state = ORES_NATIVE_ASYNC_SEND_CANCELLED;
        credit->outcome = ORES_NATIVE_SEND_CANCELLED;
    }
}

static void fail_pending_async_sends_locked(
        OresNativeActorScheduler* s, size_t actor_index,
        OresNativeSendStatus outcome) {
    ActorLane* lane = &s->actors[actor_index];
    size_t failed = 0;
    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        AsyncSendCredit* credit = &lane->async_sends[i];
        if (credit->state != ORES_NATIVE_ASYNC_SEND_PENDING)
            continue;
        settle_selectable_failure_locked(credit, outcome);
        credit->sequence = 0;
        if (credit->select_bound
                && credit->state == ORES_NATIVE_ASYNC_SEND_FAILED
                && credit->select_token.notify_scheduler_domain) {
            const OresNativeWakePublishStatus wake_status =
                publish_select_wake_locked(
                    s, credit->select_token, ORES_NATIVE_WAKE_FAILED);
            if (wake_status != ORES_NATIVE_WAKE_PUBLISHED
                    && wake_status != ORES_NATIVE_WAKE_PRODUCER_RETIRED)
                abort();
        }
        settle_async_send_wake_locked(s, credit);
        ++failed;
        if (s->rejected != UINT64_MAX) ++s->rejected;
    }
    if (failed > lane->async_pending) abort();
    lane->async_pending -= failed;
    if (failed && pthread_cond_broadcast(&s->credit) != 0) abort();
}

static void service_async_sends_locked(
        OresNativeActorScheduler* s, size_t actor_index) {
    ActorLane* lane = &s->actors[actor_index];
    if (!lane->async_pending) return;
    if (s->retired || lane->retired) {
        fail_pending_async_sends_locked(
            s, actor_index, ORES_NATIVE_SEND_TERMINATED);
        return;
    }

    while (lane->async_pending
            && lane->queued + lane->active < s->per_actor_capacity) {
        AsyncSendCredit* credit = oldest_pending_async_locked(lane);
        if (!credit) abort();

        /*
         * Preflight every resource that enqueue_locked can fail for before a
         * selectable arm commits its irreversible winner token. The scheduler
         * mutex keeps these conditions stable through the subsequent enqueue.
         */
        const int new_lane = !lane->active && !lane_has_work(lane);
        if (s->accepted >= UINT64_MAX - s->reserved_wakes
                || !execution_slot_available_locked(s)
                || !ensure_workers_locked(
                    s, s->runnable_lanes + (size_t)new_lane)) {
            fail_pending_async_sends_locked(
                s, actor_index, ORES_NATIVE_SEND_RESOURCE_EXHAUSTED);
            return;
        }

        if (credit->select_bound) {
            const OresNativeSelectCommitResult selected =
                ores_native_select_try_commit(credit->select_token);
            if (selected != ORES_NATIVE_SELECT_WON) {
                credit->state = ORES_NATIVE_ASYNC_SEND_CANCELLED;
                credit->outcome = ORES_NATIVE_SEND_CANCELLED;
                credit->sequence = 0;
                if (!lane->async_pending) abort();
                --lane->async_pending;
                settle_async_send_wake_locked(s, credit);
                continue;
            }
        }

        /*
         * After the selectable winner commits, enqueue must be infallible
         * under this same scheduler lock: target credit, execution id, worker
         * availability and scheduler liveness were all preflighted above.
         */
        if (!enqueue_locked(s, actor_index, credit->payload))
            abort();

        credit->state = ORES_NATIVE_ASYNC_SEND_READY;
        credit->outcome = ORES_NATIVE_SEND_ACCEPTED;
        credit->sequence = 0;
        if (!lane->async_pending) abort();
        --lane->async_pending;
        if (credit->select_bound
                && credit->select_token.notify_scheduler_domain) {
            const OresNativeWakePublishStatus wake_status =
                publish_select_wake_locked(
                    s, credit->select_token, ORES_NATIVE_WAKE_READY);
            if (wake_status != ORES_NATIVE_WAKE_PUBLISHED
                    && wake_status != ORES_NATIVE_WAKE_PRODUCER_RETIRED)
                abort();
        }
        settle_async_send_wake_locked(s, credit);
    }

    if (pthread_cond_broadcast(&s->credit) != 0) abort();
}
int ores_native_actor_scheduler_configure_turn_gate(
        OresNativeActorScheduler* s, OresNativePhysicalTurnGate* gate) {
    if (!s || (gate && gate->maximum == 0)) return 0;
    lock(s);
    const int ok = !s->retired && s->started_workers == 0
        && s->outstanding == 0 && s->accepted == 0;
    if (ok) s->physical_turn_gate = gate;
    unlock(s);
    return ok;
}

void ores_native_actor_scheduler_signal_turn_gate(OresNativeActorScheduler* s) {
    if (!s) return;
    lock(s);
    /* Retirement still drains already admitted mail and must receive the
     * last permit-release wake rather than parking its final worker. */
    pthread_cond_broadcast(&s->ready);
    unlock(s);
}

OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* s, size_t i) {
    OresNativeActorRef missing = {0, 0, 0};
    if (!s) return missing;
    lock(s);
    if (s->retired || i >= s->actor_count || s->actors[i].spawned) {
        unlock(s);
        return missing;
    }
    ActorLane* a = &s->actors[i];
    a->spawned = 1;
    a->generation = a->authority.generation;
    a->allocation_domain = (OresNativeAllocationDomainRef){
        ORES_ALLOCATION_DOMAIN_ACTOR,
        s->domain,
        (uint64_t)i,
        a->generation
    };
    OresNativeActorRef ref = {s->domain, (uint64_t)i, a->generation};
    unlock(s);
    return ref;
}
OresNativeSendStatus ores_native_actor_scheduler_try_send(
        OresNativeActorScheduler* s, OresNativeActorRef actor, uint64_t payload) {
    if (!s) return ORES_NATIVE_SEND_INVALID_REF;
    lock(s);
    OresNativeSendStatus status;
    int counted_in_enqueue = 0;
    if (!matching(s, actor)) {
        status = ORES_NATIVE_SEND_INVALID_REF;
    } else {
        ActorLane* lane = &s->actors[actor.actor_slot];
        if (s->retired || lane->retired) {
            status = ORES_NATIVE_SEND_TERMINATED;
        } else {
            /* Older async producers own first refusal on any free target
             * credit. A racing synchronous sender may never steal it. */
            service_async_sends_locked(s, (size_t)actor.actor_slot);
            if (lane->queued + lane->active >= s->per_actor_capacity) {
                status = ORES_NATIVE_SEND_MAILBOX_FULL;
            } else if (s->accepted >= UINT64_MAX - s->reserved_wakes
                    || !execution_slot_available_locked(s)) {
                status = ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
            } else if (enqueue_locked(
                    s, (size_t)actor.actor_slot, payload)) {
                status = ORES_NATIVE_SEND_ACCEPTED;
            } else {
                /* enqueue_locked already counted this rejected admission. */
                counted_in_enqueue = 1;
                status = ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
            }
        }
    }
    if (status != ORES_NATIVE_SEND_ACCEPTED && !counted_in_enqueue
            && s->rejected != UINT64_MAX)
        ++s->rejected;
    unlock(s);
    return status;
}
OresNativeSendStatus ores_native_actor_scheduler_wait_send(
        OresNativeActorScheduler* s, OresNativeActorRef actor, uint64_t payload) {
    if (!s) return ORES_NATIVE_SEND_INVALID_REF;
    /* Compatibility host bridge only. A carrier of ANY scheduler must not
     * park here; cooperative producer futures use submit_async instead. */
    if (executing != NULL) return ORES_NATIVE_SEND_TRANSPORT_UNAVAILABLE;

    lock(s);
    for (;;) {
        OresNativeSendStatus status = ORES_NATIVE_SEND_INVALID_REF;
        int counted_in_enqueue = 0;
        if (!matching(s, actor)) {
            status = ORES_NATIVE_SEND_INVALID_REF;
        } else {
            ActorLane* lane = &s->actors[actor.actor_slot];
            if (s->retired || lane->retired) {
                status = ORES_NATIVE_SEND_TERMINATED;
            } else {
                service_async_sends_locked(s, (size_t)actor.actor_slot);
                if (lane->queued + lane->active >= s->per_actor_capacity) {
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
                    ++lane->test_waiting_sends;
#endif
                    if (pthread_cond_wait(&s->credit, &s->lock) != 0) abort();
#ifdef ORESLANG_SOURCE_ACTOR_TESTING
                    if (!lane->test_waiting_sends) abort();
                    --lane->test_waiting_sends;
#endif
                    continue;
                }
                if (s->accepted >= UINT64_MAX - s->reserved_wakes
                        || !execution_slot_available_locked(s)) {
                    status = ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
                } else if (enqueue_locked(
                        s, (size_t)actor.actor_slot, payload)) {
                    status = ORES_NATIVE_SEND_ACCEPTED;
                } else {
                    counted_in_enqueue = 1;
                    status = ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
                }
            }
        }

        if (status != ORES_NATIVE_SEND_ACCEPTED && !counted_in_enqueue
                && s->rejected != UINT64_MAX)
            ++s->rejected;
        unlock(s);
        return status;
    }
}
OresNativeSendStatus ores_native_actor_scheduler_submit_async(
        OresNativeActorScheduler* s, OresNativeActorRef target,
        uint64_t payload, OresNativeAsyncSendTicket* out) {
    if (!out) return ORES_NATIVE_SEND_INVALID_REF;
    *out = (OresNativeAsyncSendTicket){0};
    if (!s) return ORES_NATIVE_SEND_INVALID_REF;

    lock(s);
    if (!matching(s, target)) {
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return ORES_NATIVE_SEND_INVALID_REF;
    }

    ActorLane* lane = &s->actors[target.actor_slot];
    if (s->retired || lane->retired) {
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return ORES_NATIVE_SEND_TERMINATED;
    }

    service_async_sends_locked(s, (size_t)target.actor_slot);

    if (lane->async_live >= ORES_NATIVE_ASYNC_SENDS_PER_ACTOR) {
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
    }

    size_t slot = ORES_NATIVE_ASYNC_SENDS_PER_ACTOR;
    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        AsyncSendCredit* candidate = &lane->async_sends[i];
        if (candidate->state == ORES_NATIVE_ASYNC_SEND_INVALID
                && candidate->generation != UINT64_MAX) {
            slot = i;
            break;
        }
    }
    if (slot == ORES_NATIVE_ASYNC_SENDS_PER_ACTOR) {
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
    }

    const int must_wait = lane->async_pending != 0
        || lane->queued + lane->active >= s->per_actor_capacity;
    if (must_wait && lane->next_async_sequence == 0) {
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
    }

    /*
     * Only MAILBOX_FULL is asynchronous. If target credit is immediately
     * available, every other admission failure (execution id, carrier growth,
     * scheduler resource exhaustion) must propagate synchronously and must not
     * mint a Future descriptor. The scheduler mutex keeps the chosen free
     * descriptor slot stable across this admission attempt.
     */
    if (!must_wait
            && !enqueue_locked(s, (size_t)target.actor_slot, payload)) {
        unlock(s);
        return ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
    }

    AsyncSendCredit* credit = &lane->async_sends[slot];
    ++credit->generation;
    if (!credit->generation) abort();
    credit->payload = payload;
    credit->sequence = 0;
    credit->wake = (OresNativeWakeTicket){0};
    credit->wake_bound = 0;
    credit->select_token = (OresNativeSelectToken){0};
    credit->select_bound = 0;
    credit->release_pending = 0;
    ++lane->async_live;

    *out = (OresNativeAsyncSendTicket){
        .target = target,
        .slot = slot,
        .generation = credit->generation
    };

    if (!must_wait) {
        credit->state = ORES_NATIVE_ASYNC_SEND_READY;
        credit->outcome = ORES_NATIVE_SEND_ACCEPTED;
    } else {
        credit->state = ORES_NATIVE_ASYNC_SEND_PENDING;
        credit->outcome = ORES_NATIVE_SEND_MAILBOX_FULL;
        credit->sequence = lane->next_async_sequence;
        lane->next_async_sequence = lane->next_async_sequence == UINT64_MAX
            ? 0 : lane->next_async_sequence + 1;
        ++lane->async_pending;
    }

    if (pthread_cond_broadcast(&s->credit) != 0) abort();
    unlock(s);
    /* ACCEPTED means Future creation succeeded; READY means message admission. */
    return ORES_NATIVE_SEND_ACCEPTED;
}

OresNativeSendStatus ores_native_actor_scheduler_submit_selectable_async(
        OresNativeActorScheduler* s, OresNativeActorRef target,
        uint64_t payload, OresNativeSelectToken select_token,
        OresNativeAsyncSendTicket* out) {
    if (!out) return ORES_NATIVE_SEND_INVALID_REF;
    *out = (OresNativeAsyncSendTicket){0};
    if (!s || !select_token.arbiter
            || !ores_native_select_token_registered(select_token))
        return ORES_NATIVE_SEND_INVALID_REF;

    lock(s);

    /*
     * The common winner may have committed after the caller's precheck but
     * before this scheduler acquired its target-credit lock. Detach this arm
     * without minting any async descriptor in that case.
     */
    if (!ores_native_select_token_registered(select_token)) {
        (void)ores_native_select_detach(select_token);
        unlock(s);
        return ORES_NATIVE_SEND_CANCELLED;
    }

    OresNativeSendStatus immediate_terminal = ORES_NATIVE_SEND_ACCEPTED;
    if (!matching(s, target))
        immediate_terminal = ORES_NATIVE_SEND_INVALID_REF;
    else if (s->retired || s->actors[target.actor_slot].retired)
        immediate_terminal = ORES_NATIVE_SEND_TERMINATED;

    if (immediate_terminal != ORES_NATIVE_SEND_ACCEPTED) {
        const OresNativeSelectCommitResult selected =
            ores_native_select_try_commit(select_token);
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return selected == ORES_NATIVE_SELECT_WON
            ? immediate_terminal
            : ORES_NATIVE_SEND_CANCELLED;
    }

    ActorLane* lane = &s->actors[target.actor_slot];

    /*
     * Descriptor/sequence exhaustion means this select arm could not be
     * registered. It is not an operation-completion event and must not win the
     * common selection. Detach this arm and let the coordinator fail/cleanup
     * the partial select setup.
     */
    if (lane->async_live >= ORES_NATIVE_ASYNC_SENDS_PER_ACTOR
            || lane->next_async_sequence == 0) {
        (void)ores_native_select_detach(select_token);
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
    }
    size_t slot = ORES_NATIVE_ASYNC_SENDS_PER_ACTOR;
    for (size_t i = 0; i < ORES_NATIVE_ASYNC_SENDS_PER_ACTOR; ++i) {
        AsyncSendCredit* candidate = &lane->async_sends[i];
        if (candidate->state == ORES_NATIVE_ASYNC_SEND_INVALID
                && candidate->generation != UINT64_MAX) {
            slot = i;
            break;
        }
    }
    if (slot == ORES_NATIVE_ASYNC_SENDS_PER_ACTOR) {
        (void)ores_native_select_detach(select_token);
        if (s->rejected != UINT64_MAX) ++s->rejected;
        unlock(s);
        return ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
    }

    AsyncSendCredit* credit = &lane->async_sends[slot];
    ++credit->generation;
    if (!credit->generation) abort();
    credit->payload = payload;
    credit->sequence = lane->next_async_sequence;
    lane->next_async_sequence = lane->next_async_sequence == UINT64_MAX
        ? 0 : lane->next_async_sequence + 1;
    credit->wake = (OresNativeWakeTicket){0};
    credit->wake_bound = 0;
    credit->select_token = select_token;
    credit->select_bound = 1;
    credit->release_pending = 0;
    credit->state = ORES_NATIVE_ASYNC_SEND_PENDING;
    credit->outcome = ORES_NATIVE_SEND_MAILBOX_FULL;
    ++lane->async_live;
    ++lane->async_pending;

    *out = (OresNativeAsyncSendTicket){
        .target = target,
        .slot = slot,
        .generation = credit->generation
    };

    /*
     * If target credit is already free this call may settle immediately, but
     * only service_async_sends_locked may attempt the select CAS and enqueue.
     * Thus winner commit and message publication share this scheduler critical
     * section and no losing arm can publish a payload.
     */
    service_async_sends_locked(s, (size_t)target.actor_slot);
    if (pthread_cond_broadcast(&s->credit) != 0) abort();
    unlock(s);
    return ORES_NATIVE_SEND_ACCEPTED;
}

int ores_native_actor_scheduler_async_send_status(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket,
        OresNativeAsyncSendState* state_out,
        OresNativeSendStatus* outcome_out) {
    if (!s || !state_out || !outcome_out) return 0;
    lock(s);
    AsyncSendCredit* credit = matching_async_send_locked(s, ticket);
    if (!credit) {
        unlock(s);
        return 0;
    }
    *state_out = credit->state;
    *outcome_out = credit->outcome;
    unlock(s);
    return 1;
}

int ores_native_actor_scheduler_await_async_send(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket,
        OresNativeSendStatus* outcome_out) {
    if (!s || !outcome_out || executing != NULL) return 0;
    lock(s);
    for (;;) {
        AsyncSendCredit* credit = matching_async_send_locked(s, ticket);
        if (!credit) {
            unlock(s);
            return 0;
        }
        if (credit->state != ORES_NATIVE_ASYNC_SEND_PENDING) {
            *outcome_out = credit->outcome;
            unlock(s);
            return 1;
        }
        if (pthread_cond_wait(&s->credit, &s->lock) != 0) abort();
    }
}

int ores_native_actor_scheduler_cancel_async_send(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket) {
    if (!s) return 0;
    lock(s);
    AsyncSendCredit* credit = matching_async_send_locked(s, ticket);
    const int ok = credit && credit->state == ORES_NATIVE_ASYNC_SEND_PENDING;
    if (ok) {
        ActorLane* lane = &s->actors[ticket.target.actor_slot];
        if (!lane->async_pending) abort();
        if (credit->select_bound)
            (void)ores_native_select_detach(credit->select_token);
        --lane->async_pending;
        credit->state = ORES_NATIVE_ASYNC_SEND_CANCELLED;
        credit->outcome = ORES_NATIVE_SEND_CANCELLED;
        credit->sequence = 0;
        settle_async_send_wake_locked(s, credit);
        service_async_sends_locked(s, (size_t)ticket.target.actor_slot);
        if (pthread_cond_broadcast(&s->credit) != 0) abort();
    }
    unlock(s);
    return ok;
}

int ores_native_actor_scheduler_release_async_send(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket) {
    /* Physical descriptor retirement may wait for an executing wake callback.
     * A carrier must never park here, including a foreign scheduler carrier. */
    if (!s || executing != NULL) return 0;
    lock(s);
    AsyncSendCredit* credit = matching_async_send_locked(s, ticket);
    int ok = credit
        && credit->state != ORES_NATIVE_ASYNC_SEND_PENDING
        && credit->state != ORES_NATIVE_ASYNC_SEND_INVALID
        && !credit->release_pending;
    if (ok) {
        WakeCredit* bound_wake = credit->wake_bound
            ? matching_wake_locked(s, credit->wake) : NULL;
        if (!bound_wake) {
            retire_async_send_credit_locked(s, ticket, credit);
        } else if (bound_wake->state == WAKE_EXECUTING) {
            /* Claim release exactly once, then wait for the carrier to return
             * from the callback and retire this exact generation. cond_wait
             * drops the scheduler mutex, so the carrier can make progress. */
            credit->release_pending = 1;
            while (matching_async_send_locked(s, ticket) != NULL)
                if (pthread_cond_wait(&s->credit, &s->lock) != 0) abort();
        } else {
            /* RESERVED/TRANSFERRED/READY still has a future observer that may
             * read descriptor identity or completion_code. */
            ok = 0;
        }
    }
    unlock(s);
    return ok;
}

int ores_native_actor_scheduler_is_carrier_thread(void) {
    return executing != NULL;
}

#ifdef ORESLANG_NATIVE_TEST_EXECUTION_EXHAUSTION
int ores_native_actor_scheduler_test_async_release_pending(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket) {
    if (!s) return 0;
    lock(s);
    AsyncSendCredit* credit = matching_async_send_locked(s, ticket);
    const int pending = credit && credit->release_pending;
    unlock(s);
    return pending;
}
#endif

#ifdef ORESLANG_SOURCE_ACTOR_TESTING
size_t ores_native_actor_scheduler_test_waiting_sends(
        OresNativeActorScheduler* s, OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    const size_t count = matching(s, actor)
        ? s->actors[actor.actor_slot].test_waiting_sends : 0;
    unlock(s);
    return count;
}
size_t ores_native_actor_scheduler_test_pending_async_sends(
        OresNativeActorScheduler* s, OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    const size_t count = matching(s, actor)
        ? s->actors[actor.actor_slot].async_pending : 0;
    unlock(s);
    return count;
}
#endif

int ores_native_actor_scheduler_send(OresNativeActorScheduler* s,
                                     OresNativeActorRef actor, uint64_t payload) {
    return ores_native_actor_scheduler_try_send(s, actor, payload)
        == ORES_NATIVE_SEND_ACCEPTED;
}
int ores_native_actor_scheduler_submit(OresNativeActorScheduler* s,
                                       size_t i, uint64_t message) {
    if (!s) return 0;
    lock(s);
    int ok = enqueue_locked(s, i, message);
    unlock(s);
    return ok;
}
/* Bind each escrow credit to the producer's exact incarnation and native
 * continuation, target generation, code image and send descriptor. A stale
 * ticket cannot reclaim a newly reused slot. Only the scheduler authorizes
 * execution when its worker claims a READY wake. */
static WakeCredit* matching_wake_locked(
        OresNativeActorScheduler* s, OresNativeWakeTicket t) {
    /* Wake mutation APIs are part of the public trusted-host ABI and may be
     * called on a scheduler whose preallocated wake escrow was never activated.
     * Fail closed unless a trusted bootstrap installed the callback. */
    if (!s->wake_callback || !matching(s, t.producer)
            || t.slot >= ORES_NATIVE_WAKE_CREDITS_PER_ACTOR)
        return NULL;
    ActorLane* lane = &s->actors[t.producer.actor_slot];
    if (!lane->wakes) return NULL;
    WakeCredit* w = &lane->wakes[t.slot];
    const OresNativeWakeTicket* old = &w->ticket;
    if (w->state == WAKE_EMPTY || !t.slot_generation
            || w->generation != t.slot_generation
            || old->target.scheduler_domain != t.target.scheduler_domain
            || old->target.actor_slot != t.target.actor_slot
            || old->target.generation != t.target.generation
            || old->continuation_slot != t.continuation_slot
            || old->continuation_generation != t.continuation_generation
            || old->code_generation != t.code_generation
            || old->descriptor_generation != t.descriptor_generation
            || old->descriptor_slot != t.descriptor_slot)
        return NULL;
    return w;
}

static WakeCredit* matching_select_wake_locked(
        OresNativeActorScheduler* s,
        OresNativeSelectToken token) {
    if (!s || !token.notify_scheduler_domain
            || token.notify_scheduler_domain != s->domain
            || token.notify_producer_slot >= s->actor_count
            || token.notify_wake_slot >= ORES_NATIVE_WAKE_CREDITS_PER_ACTOR)
        return NULL;

    ActorLane* producer = &s->actors[token.notify_producer_slot];
    if (!producer->spawned
            || producer->generation != token.notify_producer_generation
            || !producer->wakes)
        return NULL;

    WakeCredit* wake = &producer->wakes[token.notify_wake_slot];
    if (wake->state == WAKE_EMPTY
            || wake->generation != token.notify_wake_generation
            || !wake->select_bound
            || wake->select_arbiter != token.arbiter
            || wake->select_generation != token.selection_generation
            || wake->ticket.producer.scheduler_domain != s->domain
            || wake->ticket.producer.actor_slot != token.notify_producer_slot
            || wake->ticket.producer.generation
                != token.notify_producer_generation
            || wake->ticket.descriptor_slot != UINT64_MAX
            || wake->ticket.descriptor_generation
                != token.selection_generation)
        return NULL;
    return wake;
}

static OresNativeWakePublishStatus publish_select_wake_locked(
        OresNativeActorScheduler* s,
        OresNativeSelectToken token,
        OresNativeWakeOutcome outcome) {
    if (outcome != ORES_NATIVE_WAKE_READY
            && outcome != ORES_NATIVE_WAKE_CANCELLED
            && outcome != ORES_NATIVE_WAKE_FAILED)
        return ORES_NATIVE_WAKE_INVALID;
    if (!ores_native_select_token_won(token))
        return ORES_NATIVE_WAKE_INVALID;

    WakeCredit* wake = matching_select_wake_locked(s, token);
    if (!wake || wake->state != WAKE_RESERVED)
        return ORES_NATIVE_WAKE_INVALID;

    ActorLane* producer = &s->actors[token.notify_producer_slot];
    if (s->retired || producer->retired) {
        release_wake_locked(
            s, (size_t)token.notify_producer_slot, wake);
        return ORES_NATIVE_WAKE_PRODUCER_RETIRED;
    }

    if (!s->reserved_wakes || s->accepted == UINT64_MAX) abort();
    const int new_lane = !producer->active && !lane_has_work(producer);
    --s->reserved_wakes;
    wake->ticket.completion_code = token.arm + UINT64_C(1);
    wake->outcome = outcome;
    wake->state = WAKE_READY;
    ++producer->wake_ready;
    if (new_lane) ++s->runnable_lanes;
    ++s->reserved_executions;
    ++s->accepted;
    ++s->outstanding;
    pthread_cond_signal(&s->ready);
    return ORES_NATIVE_WAKE_PUBLISHED;
}

static OresNativeWakeOutcome async_send_terminal_wake_outcome(
        const AsyncSendCredit* credit) {
    if (!credit) abort();
    switch (credit->state) {
        case ORES_NATIVE_ASYNC_SEND_READY:
            if (credit->outcome != ORES_NATIVE_SEND_ACCEPTED) abort();
            return ORES_NATIVE_WAKE_READY;
        case ORES_NATIVE_ASYNC_SEND_FAILED:
            /* These are the only post-submission failure sources today:
             * target/scheduler termination, or failure to reserve execution
             * capacity when queued mailbox credit later becomes available. */
            if (credit->outcome != ORES_NATIVE_SEND_TERMINATED
                    && credit->outcome != ORES_NATIVE_SEND_RESOURCE_EXHAUSTED)
                abort();
            return ORES_NATIVE_WAKE_FAILED;
        case ORES_NATIVE_ASYNC_SEND_CANCELLED:
            if (credit->outcome != ORES_NATIVE_SEND_CANCELLED)
                abort();
            return ORES_NATIVE_WAKE_CANCELLED;
        default:
            abort();
    }
}

static void settle_async_send_wake_locked(
        OresNativeActorScheduler* s, AsyncSendCredit* credit) {
    if (!credit || !credit->wake_bound) return;
    WakeCredit* w = matching_wake_locked(s, credit->wake);
    if (!w || w->state != WAKE_RESERVED || credit->release_pending) abort();

    const OresNativeWakeOutcome wake_outcome =
        async_send_terminal_wake_outcome(credit);
    if (wake_outcome == ORES_NATIVE_WAKE_READY) {
        /* Mailbox admission already linearized under this scheduler mutex.
         * Record irreversible transfer before publishing the producer wake. */
        w->state = WAKE_TRANSFERRED;
    }

    ActorLane* producer = &s->actors[credit->wake.producer.actor_slot];
    if (s->retired || producer->retired) {
        release_wake_locked(
            s, (size_t)credit->wake.producer.actor_slot, w);
        return;
    }

    if (!s->reserved_wakes || s->accepted == UINT64_MAX) abort();
    const int new_lane = !producer->active && !lane_has_work(producer);
    --s->reserved_wakes;
    w->ticket.completion_code = (uint64_t)credit->outcome;
    w->outcome = wake_outcome;
    w->state = WAKE_READY;
    ++producer->wake_ready;
    if (new_lane) ++s->runnable_lanes;
    ++s->reserved_executions;
    ++s->accepted;
    ++s->outstanding;
    pthread_cond_signal(&s->ready);
}
static int reserve_wake_locked_impl(
        OresNativeActorScheduler* s, OresNativeActorRef producer,
        OresNativeActorRef target, uint64_t continuation_slot,
        uint64_t continuation_generation, uint64_t code_generation,
        uint64_t descriptor_slot, uint64_t descriptor_generation,
        int require_live_target, OresNativeWakeTicket* out) {
    int ok = !s->retired && s->wake_callback && matching(s, producer)
        && (!require_live_target || target.scheduler_domain != s->domain
            || (matching(s, target) && !s->actors[target.actor_slot].retired))
        && !s->actors[producer.actor_slot].retired
        && s->reserved_wakes < UINT64_MAX
        && s->accepted < UINT64_MAX - s->reserved_wakes
        && execution_slot_available_locked(s);
    WakeCredit* w = NULL;
    ActorLane* lane = NULL;
    if (ok) {
        lane = &s->actors[producer.actor_slot];
        for (size_t i = 0; i < ORES_NATIVE_WAKE_CREDITS_PER_ACTOR; ++i) {
            if (lane->wakes[i].state == WAKE_EMPTY
                    && lane->wakes[i].generation != UINT64_MAX) {
                w = &lane->wakes[i];
                break;
            }
        }
        ok = w && ensure_workers_locked(s, s->worker_count);
    }
    if (!ok) return 0;

    ++w->generation;
    const size_t slot = (size_t)(w - lane->wakes);
    w->select_arbiter = NULL;
    w->select_generation = 0;
    w->select_bound = 0;
    w->ticket = (OresNativeWakeTicket){
        .producer = producer, .target = target,
        .slot = slot, .slot_generation = w->generation,
        .continuation_slot = continuation_slot,
        .continuation_generation = continuation_generation,
        .code_generation = code_generation,
        .descriptor_generation = descriptor_generation,
        .descriptor_slot = descriptor_slot,
        .completion_code = 0
    };
    w->state = WAKE_RESERVED;
    ++s->reserved_wakes;
    ++lane->wake_live;
    *out = w->ticket;
    return 1;
}

int ores_native_actor_scheduler_reserve_wake(
        OresNativeActorScheduler* s, OresNativeActorRef producer,
        OresNativeActorRef target, uint64_t continuation_slot,
        uint64_t continuation_generation, uint64_t code_generation,
        uint64_t descriptor_generation, OresNativeWakeTicket* out) {
    if (!s || !out) return 0;
    *out = (OresNativeWakeTicket){0};
    if (!target.scheduler_domain || !target.generation || !continuation_slot
            || !continuation_generation || !code_generation || !descriptor_generation)
        return 0;
    lock(s);
    const int ok = reserve_wake_locked_impl(
        s, producer, target, continuation_slot, continuation_generation,
        code_generation, UINT64_MAX, descriptor_generation, 1, out);
    unlock(s);
    return ok;
}

static int select_generation_has_live_wake_locked(
        OresNativeActorScheduler* s,
        OresNativeSelectArbiter* arbiter,
        uint64_t selection_generation) {
    if (!s || !arbiter || !selection_generation) return 1;
    for (size_t actor = 0; actor < s->actor_count; ++actor) {
        ActorLane* lane = &s->actors[actor];
        if (!lane->wakes) continue;
        for (size_t slot = 0;
                slot < ORES_NATIVE_WAKE_CREDITS_PER_ACTOR; ++slot) {
            WakeCredit* wake = &lane->wakes[slot];
            if (wake->state != WAKE_EMPTY
                    && wake->select_bound
                    && wake->select_arbiter == arbiter
                    && wake->select_generation == selection_generation)
                return 1;
        }
    }
    return 0;
}

static int reserve_select_wake_locked_impl(
        OresNativeActorScheduler* s,
        OresNativeSelectArbiter* arbiter,
        size_t arm_count,
        OresNativeActorRef producer,
        uint64_t continuation_slot,
        uint64_t continuation_generation,
        uint64_t code_generation,
        int rearm,
        OresNativeWakeTicket* out) {
    if (!s || !arbiter || !out || !arm_count
            || arm_count > ORES_NATIVE_SELECT_MAX_ARMS
            || !continuation_slot || !continuation_generation
            || !code_generation)
        return 0;

    const uint64_t generation = s->next_select_generation;
    if (!generation || generation == UINT64_MAX)
        return 0;

    if (rearm) {
        const uint64_t previous_generation = atomic_load_explicit(
            &arbiter->generation, memory_order_acquire);
        if (!previous_generation
                || select_generation_has_live_wake_locked(
                    s, arbiter, previous_generation))
            return 0;
    }

    OresNativeWakeTicket wake = {0};
    if (!reserve_wake_locked_impl(
            s, producer, producer,
            continuation_slot, continuation_generation, code_generation,
            UINT64_MAX, generation, 1, &wake))
        return 0;

    const int arbiter_ok = rearm
        ? ores_native_select_arbiter_rearm(arbiter, generation, arm_count)
        : ores_native_select_arbiter_init(arbiter, generation, arm_count);
    if (!arbiter_ok) {
        WakeCredit* credit = matching_wake_locked(s, wake);
        if (!credit || credit->state != WAKE_RESERVED) abort();
        release_wake_locked(
            s, (size_t)wake.producer.actor_slot, credit);
        return 0;
    }

    WakeCredit* select_wake = matching_wake_locked(s, wake);
    if (!select_wake || select_wake->state != WAKE_RESERVED)
        abort();
    select_wake->select_arbiter = arbiter;
    select_wake->select_generation = generation;
    select_wake->select_bound = 1;

    s->next_select_generation =
        generation == UINT64_MAX - UINT64_C(1)
            ? UINT64_MAX
            : generation + UINT64_C(1);
    *out = wake;
    return 1;
}

int ores_native_actor_scheduler_reserve_select_wake(
        OresNativeActorScheduler* s,
        OresNativeSelectArbiter* arbiter,
        size_t arm_count,
        OresNativeActorRef producer,
        uint64_t continuation_slot,
        uint64_t continuation_generation,
        uint64_t code_generation,
        OresNativeWakeTicket* out) {
    if (out) *out = (OresNativeWakeTicket){0};
    if (!s || !out) return 0;
    lock(s);
    const int ok = reserve_select_wake_locked_impl(
        s, arbiter, arm_count, producer,
        continuation_slot, continuation_generation, code_generation,
        0, out);
    unlock(s);
    return ok;
}

int ores_native_actor_scheduler_rearm_select_wake(
        OresNativeActorScheduler* s,
        OresNativeSelectArbiter* arbiter,
        size_t arm_count,
        OresNativeActorRef producer,
        uint64_t continuation_slot,
        uint64_t continuation_generation,
        uint64_t code_generation,
        OresNativeWakeTicket* out) {
    if (out) *out = (OresNativeWakeTicket){0};
    if (!s || !out) return 0;
    lock(s);
    const int ok = reserve_select_wake_locked_impl(
        s, arbiter, arm_count, producer,
        continuation_slot, continuation_generation, code_generation,
        1, out);
    unlock(s);
    return ok;
}

int ores_native_actor_scheduler_attach_select_wake(
        OresNativeActorScheduler* s,
        OresNativeSelectToken* token,
        OresNativeWakeTicket wake) {
    if (!s || !token) return 0;
    lock(s);
    WakeCredit* credit = matching_wake_locked(s, wake);
    const int ok = credit
        && credit->state == WAKE_RESERVED
        && credit->select_bound
        && credit->select_arbiter == token->arbiter
        && credit->select_generation == token->selection_generation
        && wake.descriptor_slot == UINT64_MAX
        && wake.descriptor_generation == token->selection_generation
        && wake.producer.scheduler_domain == s->domain
        && wake.target.scheduler_domain == wake.producer.scheduler_domain
        && wake.target.actor_slot == wake.producer.actor_slot
        && wake.target.generation == wake.producer.generation
        && ores_native_select_token_registered(*token);
    if (ok) {
        token->notify_scheduler_domain = s->domain;
        token->notify_producer_slot = wake.producer.actor_slot;
        token->notify_producer_generation = wake.producer.generation;
        token->notify_wake_slot = wake.slot;
        token->notify_wake_generation = wake.slot_generation;
    }
    unlock(s);
    return ok;
}

OresNativeWakePublishStatus ores_native_actor_scheduler_publish_select_wake(
        OresNativeActorScheduler* s,
        OresNativeSelectToken token,
        OresNativeWakeOutcome outcome) {
    if (!s) return ORES_NATIVE_WAKE_INVALID;
    lock(s);
    const OresNativeWakePublishStatus status =
        publish_select_wake_locked(s, token, outcome);
    unlock(s);
    return status;
}

int ores_native_actor_scheduler_bind_async_send_wake(
        OresNativeActorScheduler* s, OresNativeAsyncSendTicket ticket,
        OresNativeActorRef producer, uint64_t continuation_slot,
        uint64_t continuation_generation, uint64_t code_generation,
        OresNativeWakeTicket* out) {
    if (!s || !out || !continuation_slot
            || !continuation_generation || !code_generation)
        return 0;
    *out = (OresNativeWakeTicket){0};

    lock(s);
    AsyncSendCredit* credit = matching_async_send_locked(s, ticket);
    int ok = credit && !credit->wake_bound && !credit->select_bound
        && credit->state != ORES_NATIVE_ASYNC_SEND_INVALID;
    if (ok) {
        OresNativeWakeTicket wake = {0};
        const int require_live_target =
            credit->state == ORES_NATIVE_ASYNC_SEND_PENDING;
        ok = reserve_wake_locked_impl(
            s, producer, ticket.target,
            continuation_slot, continuation_generation, code_generation,
            ticket.slot, ticket.generation, require_live_target, &wake);
        if (ok) {
            credit->wake = wake;
            credit->wake_bound = 1;
            *out = wake;
            if (credit->state != ORES_NATIVE_ASYNC_SEND_PENDING)
                settle_async_send_wake_locked(s, credit);
        }
    }
    unlock(s);
    return ok;
}

int ores_native_actor_scheduler_transfer_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket) {
    if (!s) return 0;
    lock(s);
    WakeCredit* w = matching_wake_locked(s, ticket);
    int ok = w && w->state == WAKE_RESERVED && !s->retired
        && !s->actors[ticket.producer.actor_slot].retired;
    /* For a same-scheduler target, transfer is the irreversible mailbox-credit
     * handoff boundary. Revalidate the target incarnation here so a STOP that
     * won after reservation cannot be converted into a fabricated admission.
     * Cross-scheduler targets require their separate verified bridge authority. */
    if (ok && ticket.target.scheduler_domain == s->domain)
        ok = matching(s, ticket.target)
            && !s->actors[ticket.target.actor_slot].retired;
    if (ok) w->state = WAKE_TRANSFERRED;
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_cancel_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket) {
    if (!s) return 0;
    lock(s);
    WakeCredit* w = matching_wake_locked(s, ticket);
    int ok = w && w->state == WAKE_RESERVED;
    if (ok) release_wake_locked(s, (size_t)ticket.producer.actor_slot, w);
    unlock(s);
    return ok;
}
OresNativeWakePublishStatus ores_native_actor_scheduler_fail_reserved_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket) {
    if (!s) return ORES_NATIVE_WAKE_INVALID;
    lock(s);
    WakeCredit* w = matching_wake_locked(s, ticket);
    if (!w || w->state != WAKE_RESERVED) {
        unlock(s);
        return ORES_NATIVE_WAKE_INVALID;
    }
    ActorLane* lane = &s->actors[ticket.producer.actor_slot];
    if (s->retired || lane->retired) {
        /* Producer STOP wins: no callback is required, but the reserved
         * physical continuation loan must still retire exactly once. */
        release_wake_locked(s, (size_t)ticket.producer.actor_slot, w);
        unlock(s);
        return ORES_NATIVE_WAKE_PRODUCER_RETIRED;
    }
    /* The execution identifier and worker availability were prepaid at
     * RESERVE. This terminal failure does not imply target admission and does
     * not inspect target liveness: target STOP is precisely why it exists. */
    if (!s->reserved_wakes || s->accepted == UINT64_MAX) abort();
    const int new_lane = !lane->active && !lane_has_work(lane);
    --s->reserved_wakes;
    w->outcome = ORES_NATIVE_WAKE_FAILED;
    w->state = WAKE_READY;
    ++lane->wake_ready;
    if (new_lane) ++s->runnable_lanes;
    ++s->reserved_executions;
    ++s->accepted;
    ++s->outstanding;
    pthread_cond_signal(&s->ready);
    unlock(s);
    return ORES_NATIVE_WAKE_PUBLISHED;
}

int ores_native_actor_scheduler_abort_transferred_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket) {
    if (!s) return 0;
    lock(s);
    WakeCredit* w = matching_wake_locked(s, ticket);
    int ok = w && w->state == WAKE_TRANSFERRED;
    if (ok) release_wake_locked(s, (size_t)ticket.producer.actor_slot, w);
    unlock(s);
    return ok;
}
OresNativeWakePublishStatus ores_native_actor_scheduler_publish_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket,
        OresNativeWakeOutcome outcome) {
    if (!s || (outcome != ORES_NATIVE_WAKE_READY
            && outcome != ORES_NATIVE_WAKE_CANCELLED
            && outcome != ORES_NATIVE_WAKE_FAILED))
        return ORES_NATIVE_WAKE_INVALID;
    lock(s);
    WakeCredit* w = matching_wake_locked(s, ticket);
    if (!w || w->state != WAKE_TRANSFERRED) {
        unlock(s);
        return ORES_NATIVE_WAKE_INVALID;
    }
    ActorLane* lane = &s->actors[ticket.producer.actor_slot];
    if (s->retired || lane->retired) {
        /* STOP wins: consume the physical wake loan without executing guest
         * code. Callers must settle the owning send descriptor as failed. */
        release_wake_locked(s, (size_t)ticket.producer.actor_slot, w);
        unlock(s);
        return ORES_NATIVE_WAKE_PRODUCER_RETIRED;
    }
    /* Admission/execution identifiers were reserved before the Future could
     * suspend. Publication is an allocation-free in-memory state change. */
    if (!s->reserved_wakes || s->accepted == UINT64_MAX) abort();
    const int new_lane = !lane->active && !lane_has_work(lane);
    --s->reserved_wakes;
    w->outcome = outcome;
    w->state = WAKE_READY;
    ++lane->wake_ready;
    if (new_lane) ++s->runnable_lanes;
    ++s->reserved_executions;
    ++s->accepted;
    ++s->outstanding;
    pthread_cond_signal(&s->ready);
    unlock(s);
    return ORES_NATIVE_WAKE_PUBLISHED;
}
int ores_native_actor_scheduler_is_current_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket) {
    if (!s || executing != s || !executing_wake_valid
            || !executing_claim.carrier
            || executing_wake.slot != ticket.slot
            || executing_wake.slot_generation != ticket.slot_generation
            || executing_wake.continuation_slot != ticket.continuation_slot
            || executing_wake.continuation_generation != ticket.continuation_generation
            || executing_wake.code_generation != ticket.code_generation
            || executing_wake.descriptor_generation != ticket.descriptor_generation
            || executing_wake.descriptor_slot != ticket.descriptor_slot
            || executing_wake.target.scheduler_domain != ticket.target.scheduler_domain
            || executing_wake.target.actor_slot != ticket.target.actor_slot
            || executing_wake.target.generation != ticket.target.generation)
        return 0;
    return ores_native_actor_scheduler_is_current_actor(s, ticket.producer);
}
size_t ores_native_actor_scheduler_outstanding_wake_loans(
        OresNativeActorScheduler* s, OresNativeActorRef producer) {
    if (!s) return 0;
    lock(s);
    size_t n = matching(s, producer) ? s->actors[producer.actor_slot].wake_live : 0;
    unlock(s);
    return n;
}
uint64_t ores_native_actor_scheduler_wakes_completed(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    uint64_t n = s->wakes_completed;
    unlock(s);
    return n;
}
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* s, OresNativeActorRef parent, uint64_t payload) {
    if (!s) return 0;
    lock(s);
    int ok = !s->retired && s->control_callback && matching(s, parent)
        && !s->actors[parent.actor_slot].retired
        && !s->actors[parent.actor_slot].has_control
        && s->accepted < UINT64_MAX - s->reserved_wakes
        && execution_slot_available_locked(s);
    if (ok) {
        ActorLane* lane = &s->actors[parent.actor_slot];
        const int new_lane = !lane->active && !lane_has_work(lane);
        if (!ensure_workers_locked(s, s->runnable_lanes + (size_t)new_lane)) {
            if (s->rejected != UINT64_MAX) ++s->rejected;
            unlock(s);
            return 0;
        }
        if (new_lane) ++s->runnable_lanes;
        lane->control_payload = payload;
        lane->control_is_failure = 0;
        lane->has_control = 1;
        ++s->reserved_executions;
        ++s->accepted;
        ++s->outstanding;
        pthread_cond_signal(&s->ready);
    } else if (s->rejected != UINT64_MAX) ++s->rejected;
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_publish_failure(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure) {
    if (!s) return 0;
    lock(s);
    int ok = s->panic_callback != NULL && !s->retired
        && matching(s, parent) && matching(s, failure.child)
        && parent.actor_slot != failure.child.actor_slot
        && failure.kind >= ORES_NATIVE_CONTROL_PANIC
        && failure.kind <= ORES_NATIVE_CONTROL_STOPPED
        && !s->actors[parent.actor_slot].retired
        && !s->actors[parent.actor_slot].has_control
        && s->accepted < UINT64_MAX - s->reserved_wakes
        && execution_slot_available_locked(s);
    if (ok) {
        ActorLane* a = &s->actors[parent.actor_slot];
        const int new_lane = !a->active && !lane_has_work(a);
        if (!ensure_workers_locked(s, s->runnable_lanes + (size_t)new_lane)) {
            if (s->rejected != UINT64_MAX) ++s->rejected;
            unlock(s);
            return 0;
        }
        if (new_lane) ++s->runnable_lanes;
        a->failure = failure;
        a->control_is_failure = 1;
        a->has_control = 1;
        s->reserved_executions++;
        s->accepted++;
        s->outstanding++;
        pthread_cond_signal(&s->ready);
    } else if (s->rejected != UINT64_MAX) s->rejected++;
    unlock(s);
    return ok;
}
uint64_t ores_native_actor_scheduler_control_completed(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    uint64_t n = s->control_completed;
    unlock(s);
    return n;
}
static int stop_locked(
        OresNativeActorScheduler* s,
        OresNativeActorRef actor,
        int abandon_queued_data) {
    if (!matching(s, actor) || s->actors[actor.actor_slot].retired)
        return 0;

    ActorLane* lane = &s->actors[actor.actor_slot];
    lane->requested_terminal_kind = ORES_NATIVE_TERMINATION_STOPPED;
    lane->retired = 1;

    /* STOP rejects future DATA admission. Pending async descriptors have not
     * crossed the mailbox-admission boundary, so fail them now. READY futures
     * remain READY: their DATA envelope was already irrevocably admitted. */
    fail_pending_async_sends_locked(
        s, (size_t)actor.actor_slot, ORES_NATIVE_SEND_TERMINATED);

    if (abandon_queued_data && lane->queued) {
        const size_t abandoned = lane->queued;
        if (s->reserved_executions < abandoned || s->outstanding < abandoned)
            abort();
        if (lane->abandoned_user_messages > UINT64_MAX - abandoned)
            abort();

        /*
         * A lane remains counted as runnable while an active turn or CONTROL
         * envelope exists. Only an idle DATA-only lane disappears from the
         * runnable-lane set when its queued user mail is sealed away.
         */
        if (!lane->active && !lane->has_control && !lane->wake_ready) {
            if (!s->runnable_lanes) abort();
            --s->runnable_lanes;
        }

        lane->head = lane->tail;
        lane->queued = 0;
        lane->abandoned_user_messages += (uint64_t)abandoned;
        s->reserved_executions -= abandoned;
        s->outstanding -= abandoned;
    }

    if (!ores_execution_stop(&lane->authority, lane->generation)) abort();
    if (pthread_cond_broadcast(&s->credit) != 0) abort();
    maybe_finish_stopped_locked(s, (size_t)actor.actor_slot);
    if (!s->outstanding) {
        /*
         * Defensive wake for accounting bugs: CONTROL/active/wake work should
         * keep outstanding nonzero once published. Reserved/transferred wake
         * loans are fenced separately by wake_live and settle terminal state
         * when released.
         */
        pthread_cond_broadcast(&s->drained);
    }
    return 1;
}

int ores_native_actor_scheduler_stop(OresNativeActorScheduler* s,
                                     OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    const int ok = stop_locked(s, actor, 0);
    unlock(s);
    return ok;
}

int ores_native_actor_scheduler_stop_sealed(OresNativeActorScheduler* s,
                                            OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    const int ok = stop_locked(s, actor, 1);
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_poll_ready(OresNativeActorScheduler* s,
                                           OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int ready = matching(s, actor) && !s->retired && !s->actors[actor.actor_slot].retired;
    unlock(s);
    return ready;
}
int ores_native_actor_scheduler_poll_done(OresNativeActorScheduler* s,
                                          OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int done = matching(s, actor) && s->actors[actor.actor_slot].terminal_valid;
    unlock(s);
    return done;
}
int ores_native_actor_scheduler_terminal_state(
        OresNativeActorScheduler* s,
        OresNativeActorRef actor,
        OresNativeActorTerminalState* output) {
    if (!s || !output) return 0;
    lock(s);
    int available = matching(s, actor)
        && s->actors[actor.actor_slot].terminal_valid;
    if (available) *output = s->actors[actor.actor_slot].terminal;
    unlock(s);
    return available;
}
int ores_native_actor_scheduler_wait_done(OresNativeActorScheduler* s,
                                          OresNativeActorRef actor) {
    /* Host-only barrier. A carrier of another scheduler can participate in
     * a dependency cycle, even when the target scheduler is distinct. */
    if (!s || executing != NULL) return 0;
    lock(s);
    if (!matching(s, actor) || !s->actors[actor.actor_slot].retired) {
        unlock(s);
        return 0;
    }
    ActorLane* lane = &s->actors[actor.actor_slot];
    while (lane_has_work(lane) || lane->active || lane->wake_live
        || ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED
        || !lane->terminal_valid) {
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    }
    unlock(s);
    return 1;
}
void ores_native_actor_scheduler_retire(OresNativeActorScheduler* s) {
    if (!s) abort();
    lock(s);
    s->retired = 1;
    for (size_t i = 0; i < s->actor_count; ++i) {
        ActorLane* lane = &s->actors[i];
        if (lane->spawned && !lane->retired)
            lane->requested_terminal_kind = ORES_NATIVE_TERMINATION_RUNTIME_RETIRED;
        lane->retired = 1;
        if (lane->spawned) {
            fail_pending_async_sends_locked(
                s, i, ORES_NATIVE_SEND_TERMINATED);
            if (ores_execution_phase(&lane->authority) != ORES_EXEC_STOPPING
                && ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED)
                if (!ores_execution_stop(&lane->authority, lane->generation)) abort();
            maybe_finish_stopped_locked(s, i);
        }
    }
    if (pthread_cond_broadcast(&s->ready) != 0) abort();
    if (pthread_cond_broadcast(&s->credit) != 0) abort();
    unlock(s);
}
int ores_native_actor_scheduler_is_current_carrier(OresNativeActorScheduler* s) {
    return s && executing == s;
}
/* Actor-specific authority, not merely membership in the same worker pool. */
int ores_native_actor_scheduler_is_current_actor(
        OresNativeActorScheduler* s, OresNativeActorRef actor) {
    if (!s || executing != s
            || !actor.scheduler_domain
            || actor.scheduler_domain != executing_actor.scheduler_domain
            || actor.actor_slot != executing_actor.actor_slot
            || actor.generation != executing_actor.generation
            || !executing_claim.carrier)
        return 0;
    // An active callback pins its scheduler/lane; only the authority mutex
    // is acquired here, never while an actor callback itself is invoked.
    return ores_execution_owns(&s->actors[actor.actor_slot].authority, executing_claim);
}
int ores_native_actor_scheduler_current_execution_context(
        OresNativeActorScheduler* s, OresNativeExecutionContext* out) {
    if (!s || !out || executing != s || !executing_context.id.runtime_domain
            || !executing_context.id.execution)
        return 0;
    if (!ores_native_actor_scheduler_is_current_actor(s, executing_actor))
        return 0;
    *out = executing_context;
    return 1;
}
int ores_native_actor_scheduler_signal_control(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    int ok = !s->retired;
    if (ok && pthread_cond_broadcast(&s->ready) != 0) abort();
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* s) {
    /* Do not allow any actor carrier to block on another scheduler's work. */
    if (!s || executing != NULL) return 0;
    lock(s);
    while (s->outstanding)
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    unlock(s);
    return 1;
}
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* s) {
    /* This may join OS threads. Destruction is external-host-only across
     * every scheduler, not merely prohibited on the target's own carriers. */
    if (!s || executing != NULL) return 0;
    lock(s);
    int ready = s->retired && !s->outstanding && !s->reserved_wakes;
    if (ready) {
        for (size_t i = 0; i < s->actor_count; ++i) {
            ActorLane* lane = &s->actors[i];
            if (lane->async_live != 0
                    || (lane->spawned
                        && (ores_execution_phase(&lane->authority)
                                != ORES_EXEC_TERMINATED
                            || !lane->terminal_valid))) {
                ready = 0;
                break;
            }
        }
    }
    if (ready) pthread_cond_broadcast(&s->ready);
    unlock(s);
    if (!ready) return 0;
    /* Caller must externally fence producers; no concurrent API calls now. */
    for (size_t i = 0; i < s->started_workers; ++i)
        if (pthread_join(s->threads[i], NULL) != 0) abort();
    if (pthread_cond_destroy(&s->credit) != 0
        || pthread_cond_destroy(&s->drained) != 0
        || pthread_cond_destroy(&s->ready) != 0
        || pthread_mutex_destroy(&s->lock) != 0) abort();
    for (size_t i = 0; i < s->actor_count; ++i)
        if (!ores_execution_destroy(&s->actors[i].authority)) abort();
    free(s->threads);
    free(s->slots);
    free(s->wake_slots);
    free(s->async_send_slots);
    free(s->actors);
    free(s);
    return 1;
}
static uint64_t counter(OresNativeActorScheduler* s, int which) {
    if (!s) abort();
    lock(s);
    uint64_t n = which == 0 ? s->accepted : which == 1 ? s->rejected : s->completed;
    unlock(s);
    return n;
}
size_t ores_native_actor_scheduler_started_carriers(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    size_t count = s->started_workers;
    unlock(s);
    return count;
}
uint64_t ores_native_actor_scheduler_accepted(OresNativeActorScheduler* s) {
    return counter(s, 0);
}
uint64_t ores_native_actor_scheduler_rejected(OresNativeActorScheduler* s) {
    return counter(s, 1);
}
uint64_t ores_native_actor_scheduler_completed(OresNativeActorScheduler* s) {
    return counter(s, 2);
}
