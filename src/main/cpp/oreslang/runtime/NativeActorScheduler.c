#if defined(__linux__) && !defined(_GNU_SOURCE)
/* cpu_set_t, pthread_attr_setaffinity_np and sched_getcpu. */
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeExecutionAuthority.h"
#include "oreslang/runtime/NativeExecutionContext.h"
#include "oreslang/runtime/NativePhysicalTurnGate.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
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
    /* Runtime-authenticated source DATA-mail at WAKE reservation, 0 for
     * outside-DATA and generic callback reservations. Never guest supplied. */
    uint64_t originating_data_mail;
} WakeCredit;
typedef struct {
    uint64_t* messages; /* fixed ring, assigned at scheduler startup */
    uint64_t* mail_sequences; /* monotonic admission ordinal per ring entry */
    size_t head, tail, queued;
    uint64_t* tasks; /* actor-owned continuation handles, never user DATA */
    size_t task_head, task_tail, task_queued;
    unsigned active;
    pthread_t carrier_owner; /* owning physical worker while active */
    unsigned carrier_owner_valid;
    unsigned has_control;
    /* One preallocated supervisor failure receipt per child incarnation. */
    OresNativeActorRef supervisor;
    unsigned supervisor_bound;
    unsigned failure_state; /* 0 unused, 1 queued, 2 executing, 3 acknowledged */
    uint32_t failure_next;
    uint32_t failure_head, failure_tail; /* intrusive per-parent FIFO */
    WakeCredit* wakes; /* preallocated producer-local escrow credits */
    size_t wake_live, wake_ready, wake_cursor;
    uint64_t control_payload;
    OresNativeActorTerminalFailure failure;
    unsigned control_is_failure;
    unsigned spawned, retired;
    uint64_t generation;
    uint64_t resource_loans; /* exact-incarnation external physical resources */
    uint64_t accepted_user_messages;
    uint64_t processed_user_messages;
    uint64_t abandoned_user_messages;
    OresNativeActorTerminationKind requested_terminal_kind;
    unsigned terminal_valid;
    OresNativeActorTerminalState terminal;
    /* Stable actor-lifetime allocation capability, independent of turn ids. */
    OresNativeAllocationDomainRef allocation_domain;
    OresNativeExecutionAuthority authority;
    /* Intrusive ready-queue links, one per turn class (see TURN_*). A lane is
     * linked into class k exactly when it is idle and has class-k work, so a
     * carrier finds its next turn in O(1) instead of scanning every lane. */
    uint32_t rq_next[4], rq_prev[4];
    unsigned char rq_in[4];
} ActorLane;
/* Turn classes double as the dispatch `kind` and as ready-queue indexes.
 * Supervisor CONTROL outranks WAKE which outranks DATA. */
enum { TURN_DATA = 0, TURN_TASK = 1, TURN_WAKE = 2, TURN_CONTROL = 3, TURN_CLASSES = 4 };
#define RQ_NIL UINT32_MAX
enum { MAX_CARRIERS = 128, MAX_IDLE_SPIN_NS = 1000000 };
typedef struct {
    OresNativeActorScheduler* scheduler;
    size_t index;
    /* Set by the carrier itself after reading back its OS affinity. */
    int affinity_observed;
} CarrierSlot;
struct OresNativeActorScheduler {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t drained;
    ActorLane* actors;
    uint64_t* slots;
    uint64_t* sequence_slots;
    uint64_t* task_slots;
    WakeCredit* wake_slots;
    pthread_t* threads;
    CarrierSlot* carrier_slots;
    size_t actor_count, per_actor_capacity, worker_count, started_workers;
    size_t outstanding, runnable_lanes;
    uint32_t rq_head[TURN_CLASSES], rq_tail[TURN_CLASSES];
    /* Carriers parked in pthread_cond_wait(ready) / busy-polling work_epoch.
     * Both change only under `lock`, which is what makes skipping a signal
     * safe: a producer that sees a spinner knows it will re-check under the
     * lock before it can park. */
    size_t idle_waiters, spinners;
    size_t carriers_checked_in; /* carriers that reached their run loop */
    /* Bumped (under lock) whenever new work or a lifecycle change may need a
     * carrier. Spinning carriers read it without the lock. */
    _Atomic uint64_t work_epoch;
    uint32_t idle_spin_ns; /* bootstrap-only; 0 = park immediately */
    int cpu_affinity_configured;
    size_t cpu_count;
    uint32_t cpus[MAX_CARRIERS];
    uint64_t domain;
    uint64_t next_execution;
    uint64_t reserved_executions;
    uint64_t reserved_wakes; /* future accepted/execution credits */
    uint64_t resource_loans; /* external resources across all actor lanes */
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
    OresNativeActorTaskFn task_callback;
    OresNativeActorTaskFn task_cancel_callback;
    void* context;
    OresNativePhysicalTurnGate* physical_turn_gate;
};
static _Thread_local OresNativeActorScheduler* executing;
static _Thread_local int executing_kind = -1;
static _Thread_local OresNativeActorRef executing_actor;
static _Thread_local OresExecutionClaim executing_claim;
static _Thread_local uint64_t executing_data_mail_sequence;
static _Thread_local OresNativeExecutionContext executing_context;
static _Thread_local OresNativeWakeTicket executing_wake;
static _Thread_local OresNativeWakeOutcome executing_wake_outcome;
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

static int init_scheduler_mutex(OresNativeActorScheduler* s,
        OresNativeSchedulerLockProtocol protocol) {
    pthread_mutexattr_t attr;
    int attr_ready = pthread_mutexattr_init(&attr) == 0;
    if (attr_ready && protocol == ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT) {
        /* Explicitly no PI: a contended PI mutex always enters the kernel. */
#if defined(__GLIBC__) && defined(PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP)
        (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
#endif
        s->priority_inheritance_enabled = 0;
        const int ok = pthread_mutex_init(&s->lock, &attr) == 0;
        if (pthread_mutexattr_destroy(&attr) != 0) abort();
        return ok;
    }
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
            || lane->failure_state == 1 || lane->failure_state == 2
            || lane->wake_live || lane->wake_ready || lane->task_queued
            || lane->resource_loans)
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
static int lane_has_work(const ActorLane* lane) {
    return lane->queued || lane->task_queued || lane->has_control || lane->wake_ready
        || lane->failure_head != RQ_NIL;
}
static int lane_wants_class(const ActorLane* lane, int k) {
    if (lane->active) return 0;
    return k == TURN_CONTROL ? (lane->has_control != 0 || lane->failure_head != RQ_NIL)
         : k == TURN_WAKE ? lane->wake_ready != 0
         : k == TURN_TASK ? lane->task_queued != 0
         : lane->queued != 0;
}
static void rq_push_locked(OresNativeActorScheduler* s, size_t i, int k) {
    ActorLane* lane = &s->actors[i];
    if (lane->rq_in[k]) abort();
    lane->rq_next[k] = RQ_NIL;
    lane->rq_prev[k] = s->rq_tail[k];
    if (s->rq_tail[k] == RQ_NIL) s->rq_head[k] = (uint32_t)i;
    else s->actors[s->rq_tail[k]].rq_next[k] = (uint32_t)i;
    s->rq_tail[k] = (uint32_t)i;
    lane->rq_in[k] = 1;
}
static void rq_remove_locked(OresNativeActorScheduler* s, size_t i, int k) {
    ActorLane* lane = &s->actors[i];
    if (!lane->rq_in[k]) abort();
    if (lane->rq_prev[k] == RQ_NIL) s->rq_head[k] = lane->rq_next[k];
    else s->actors[lane->rq_prev[k]].rq_next[k] = lane->rq_next[k];
    if (lane->rq_next[k] == RQ_NIL) s->rq_tail[k] = lane->rq_prev[k];
    else s->actors[lane->rq_next[k]].rq_prev[k] = lane->rq_prev[k];
    lane->rq_next[k] = lane->rq_prev[k] = RQ_NIL;
    lane->rq_in[k] = 0;
}
/* Re-derive lane i's ready-queue membership from its state. Call after any
 * change to active/queued/has_control/wake_ready. Returns 1 when the lane
 * became newly runnable in some class (a carrier may need waking). Newly
 * runnable lanes join at the tail, which gives round-robin fairness. */
static int rq_sync_locked(OresNativeActorScheduler* s, size_t i) {
    ActorLane* lane = &s->actors[i];
    int added = 0;
    for (int k = 0; k < TURN_CLASSES; ++k) {
        const int want = lane_wants_class(lane, k);
        if (want && !lane->rq_in[k]) {
            rq_push_locked(s, i, k);
            added = 1;
        } else if (!want && lane->rq_in[k]) {
            rq_remove_locked(s, i, k);
        }
    }
    return added;
}
static int rq_any_locked(const OresNativeActorScheduler* s) {
    return s->rq_head[TURN_CONTROL] != RQ_NIL
        || s->rq_head[TURN_WAKE] != RQ_NIL
        || s->rq_head[TURN_TASK] != RQ_NIL
        || s->rq_head[TURN_DATA] != RQ_NIL;
}
#ifdef ORESLANG_NATIVE_SCHED_VALIDATE
/* Test-only O(N) cross-check that every queue matches lane state exactly. */
static void rq_validate_locked(OresNativeActorScheduler* s) {
    for (int k = 0; k < TURN_CLASSES; ++k) {
        size_t linked = 0;
        uint32_t prev = RQ_NIL;
        for (uint32_t i = s->rq_head[k]; i != RQ_NIL; i = s->actors[i].rq_next[k]) {
            if (i >= s->actor_count || !s->actors[i].rq_in[k]
                    || s->actors[i].rq_prev[k] != prev
                    || ++linked > s->actor_count)
                abort();
            prev = i;
        }
        if (s->rq_tail[k] != prev) abort();
        size_t wanted = 0;
        for (size_t i = 0; i < s->actor_count; ++i) {
            const ActorLane* lane = &s->actors[i];
            if (!!lane->active != !!lane->carrier_owner_valid) abort();
            if (lane_wants_class(lane, k) != lane->rq_in[k]) abort();
            wanted += lane->rq_in[k];
        }
        if (wanted != linked) abort();
    }
}
#define RQ_VALIDATE(s) rq_validate_locked(s)
#else
#define RQ_VALIDATE(s) ((void)0)
#endif
/* Signal at most one parked carrier for new runnable work. A spinning
 * carrier sees the epoch bump and takes the work without a kernel wake. */
static void wake_one_locked(OresNativeActorScheduler* s) {
    atomic_fetch_add_explicit(&s->work_epoch, 1, memory_order_release);
    if (s->spinners == 0 && s->idle_waiters > 0
            && pthread_cond_signal(&s->ready) != 0)
        abort();
}
/* Lifecycle/gate changes every carrier must observe. */
static void wake_all_locked(OresNativeActorScheduler* s) {
    atomic_fetch_add_explicit(&s->work_epoch, 1, memory_order_release);
    if (pthread_cond_broadcast(&s->ready) != 0) abort();
}
/* O(1): supervisor CONTROL outranks WAKE which outranks DATA. A carrier gets
 * two consecutive DATA turns on its most recent actor (warm cache) before
 * the head of the DATA queue gets priority. */
static int find_ready(OresNativeActorScheduler* s, size_t* actor, int* kind,
        size_t recent_actor, unsigned consecutive) {
    if (s->rq_head[TURN_CONTROL] != RQ_NIL) {
        *actor = s->rq_head[TURN_CONTROL]; *kind = TURN_CONTROL;
        return 1;
    }
    if (s->rq_head[TURN_WAKE] != RQ_NIL) {
        *actor = s->rq_head[TURN_WAKE]; *kind = TURN_WAKE;
        return 1;
    }
    if (s->rq_head[TURN_TASK] != RQ_NIL) {
        *actor = s->rq_head[TURN_TASK]; *kind = TURN_TASK;
        return 1;
    }
    if (recent_actor < s->actor_count && consecutive < 2
            && !s->actors[recent_actor].active
            && s->actors[recent_actor].queued) {
        *actor = recent_actor; *kind = TURN_DATA;
        return 1;
    }
    if (s->rq_head[TURN_DATA] != RQ_NIL) {
        *actor = s->rq_head[TURN_DATA]; *kind = TURN_DATA;
        return 1;
    }
    return 0;
}
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* Busy-poll the work epoch for at most budget_ns without holding the lock.
 * Bounded by construction: a spinning carrier always parks afterwards. */
static void spin_for_work(OresNativeActorScheduler* s, uint64_t epoch,
        uint32_t budget_ns) {
    const uint64_t deadline = monotonic_ns() + budget_ns;
    for (unsigned n = 1;; ++n) {
        if (atomic_load_explicit(&s->work_epoch, memory_order_acquire) != epoch)
            return;
        cpu_relax();
        /* Read the clock only every 64 polls; it costs more than a poll. */
        if ((n & 63u) == 0 && monotonic_ns() >= deadline) return;
    }
}
static void maybe_finish_stopped_locked(OresNativeActorScheduler* s, size_t i) {
    ActorLane* lane = &s->actors[i];
    if (!lane->retired || lane->active || lane_has_work(lane)
            || lane->failure_state == 1 || lane->failure_state == 2
            || lane->wake_live || lane->resource_loans)
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
    credit->originating_data_mail = 0;
    maybe_finish_stopped_locked(s, i);
}
/* Read back this carrier's OS affinity: a successful pthread_attr call is
 * not evidence (docs/HUNGRY_ACTOR_AUDIT.md). True only when the thread is
 * restricted to exactly its configured CPU and is currently running on it. */
static int observe_carrier_affinity(const OresNativeActorScheduler* s,
        size_t index) {
#if defined(__linux__)
    if (!s->cpu_affinity_configured || !s->cpu_count) return 0;
    const uint32_t want = s->cpus[index % s->cpu_count];
    cpu_set_t actual;
    CPU_ZERO(&actual);
    if (pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual) != 0)
        return 0;
    return CPU_COUNT(&actual) == 1 && CPU_ISSET(want, &actual)
        && sched_getcpu() == (int)want;
#else
    (void)s; (void)index;
    return 0;
#endif
}
static void* carrier(void* raw) {
    CarrierSlot* slot = (CarrierSlot*)raw;
    OresNativeActorScheduler* s = slot->scheduler;
    size_t recent_actor = SIZE_MAX;
    unsigned consecutive = 0;
    const int affinity_observed = observe_carrier_affinity(s, slot->index);
    /* The lock is held at the top of every iteration: a finished turn's
     * bookkeeping and the next dispatch share one critical section. */
    lock(s);
    slot->affinity_observed = affinity_observed;
    ++s->carriers_checked_in;
    pthread_cond_broadcast(&s->drained); /* prestart waits for check-in */
    for (;;) {
        size_t i = 0;
        int kind = 0;
        int gate_held = 0;
        int spun = 0;
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
            if (!waiting_on_gate && s->idle_spin_ns && !spun) {
                /* Bounded busy-poll before parking: a handoff that lands
                 * within the budget costs a cache-line transfer instead of a
                 * kernel sleep + wake. Producers skip the cond signal while a
                 * spinner exists, which is safe because every decision below
                 * is re-made under the lock before this carrier can park. */
                const uint64_t epoch =
                    atomic_load_explicit(&s->work_epoch, memory_order_acquire);
                ++s->spinners;
                unlock(s);
                spin_for_work(s, epoch, s->idle_spin_ns);
                lock(s);
                --s->spinners;
                spun = 1;
                continue;
            }
            if (waiting_on_gate) {
                atomic_fetch_add_explicit(&gate->waiting, 1, memory_order_acq_rel);
                if (ores_native_turn_gate_try_acquire(gate)) {
                    atomic_fetch_sub_explicit(&gate->waiting, 1, memory_order_acq_rel);
                    gate_held = 1;
                    break;
                }
            }
            ++s->idle_waiters;
            if (pthread_cond_wait(&s->ready, &s->lock) != 0) abort();
            --s->idle_waiters;
            spun = 0;
            if (waiting_on_gate) {
                atomic_fetch_sub_explicit(&gate->waiting, 1, memory_order_acq_rel);
            }
        }
        consecutive = (i == recent_actor && kind == TURN_DATA)
                ? (consecutive < 2 ? consecutive + 1 : 2) : 1;
        recent_actor = i;
        ActorLane* lane = &s->actors[i];
        uint64_t payload = 0;
        uint64_t original_mail_sequence = 0;
        OresNativeActorTerminalFailure failure = {0};
        OresNativeWakeTicket wake_ticket = {0};
        OresNativeWakeOutcome wake_outcome = ORES_NATIVE_WAKE_FAILED;
        WakeCredit* active_wake = NULL;
        int typed_failure = 0;
        size_t failure_child = SIZE_MAX;
        int task_cancelled = kind == TURN_TASK && lane->retired;
        if (kind == TURN_CONTROL) {
            /* Terminal failure receipts preempt generic CONTROL and USER. */
            if (lane->failure_head != RQ_NIL) {
                failure_child = lane->failure_head;
                ActorLane* child = &s->actors[failure_child];
                if (child->failure_state != 1) abort();
                lane->failure_head = child->failure_next;
                if (lane->failure_head == RQ_NIL) lane->failure_tail = RQ_NIL;
                child->failure_next = RQ_NIL;
                child->failure_state = 2;
                failure = child->failure;
                typed_failure = 1;
            } else {
                if (!lane->has_control) abort();
                payload = lane->control_payload;
                lane->control_is_failure = 0;
                lane->has_control = 0;
            }
        } else if (kind == TURN_WAKE) {
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
        } else if (kind == TURN_TASK) {
            if (!lane->task_queued || !lane->tasks) abort();
            payload = lane->tasks[lane->task_head];
            if (++lane->task_head == ORES_NATIVE_TASK_SLOTS_PER_ACTOR)
                lane->task_head = 0;
            --lane->task_queued;
        } else {
            payload = lane->messages[lane->head];
            original_mail_sequence = lane->mail_sequences[lane->head];
            if (!original_mail_sequence) abort();
            if (++lane->head == s->per_actor_capacity) lane->head = 0;
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
        /* An active lane leaves every ready queue until its turn completes. */
        rq_sync_locked(s, i);
        RQ_VALIDATE(s);
        /* Chain wake: if other lanes are still runnable, make sure another
         * carrier is coming for them instead of waiting for this turn. */
        if (rq_any_locked(s)) wake_one_locked(s);
        unlock(s);

        // Actor identity is authorized only by the physical execution claim.
        executing_actor = (OresNativeActorRef){s->domain, (uint64_t)i, lane->generation};
        executing_claim = execution_claim;
        executing_data_mail_sequence = kind == TURN_DATA ? original_mail_sequence : 0;
        executing_context = (OresNativeExecutionContext){
            .id = {s->domain, execution_id},
            .allocation = lane->allocation_domain,
            .parent_execution = {0, 0},
            .continuation = {0, 0}
        };
        executing = s;
        executing_kind = kind;
        executing_wake_valid = kind == TURN_WAKE;
        executing_wake = wake_ticket;
        executing_wake_outcome = kind == TURN_WAKE
                ? wake_outcome : ORES_NATIVE_WAKE_FAILED;
        if (typed_failure) s->panic_callback(s->context, i, failure);
        else if (kind == TURN_CONTROL)
            s->control_callback(s->context, i, payload);
        else if (kind == TURN_WAKE)
            s->wake_callback(s->context, i, wake_ticket, wake_outcome);
        else if (kind == TURN_TASK) {
            /* Already admitted tasks must receive cleanup, not guest code,
             * when STOP won before their physical turn. */
            if (task_cancelled)
                s->task_cancel_callback(s->context, i, payload);
            else s->task_callback(s->context, i, payload);
        } else s->callback(s->context, i, payload);
        executing_wake_valid = 0;
        executing_wake = (OresNativeWakeTicket){0};
        executing_wake_outcome = ORES_NATIVE_WAKE_FAILED;
        executing_actor = (OresNativeActorRef){0, 0, 0};
        executing_claim = (OresExecutionClaim){0};
        executing_data_mail_sequence = 0;
        executing_context = (OresNativeExecutionContext){0};
        executing = NULL;
        executing_kind = -1;

        lock(s);
        if (!lane->active || !lane->carrier_owner_valid
            || !pthread_equal(lane->carrier_owner, pthread_self())
            || !s->outstanding || s->completed == UINT64_MAX) abort();
        /* Leave the generation-checked guest execution claim first, but
         * keep lane.active/outstanding until the gate has completed its
         * cross-domain wakeup. Otherwise drain() may report quiescence and
         * the VM may destroy a scheduler while a gate-release callback is
         * still trying to signal that scheduler. Without a gate there is
         * nothing to release, so the turn finishes in this same section. */
        if (!ores_execution_leave(&lane->authority, execution_claim, 0)) abort();
        if (gate_held) {
            unlock(s);
            ores_native_turn_gate_release(s->physical_turn_gate);
            lock(s);
            if (!lane->active || !lane->carrier_owner_valid
                || !pthread_equal(lane->carrier_owner, pthread_self())
                || !s->outstanding || s->completed == UINT64_MAX) abort();
        }
        lane->active = 0;
        lane->carrier_owner_valid = 0;
        s->outstanding--;
        s->completed++;
        if (kind == TURN_DATA) {
            if (lane->processed_user_messages == UINT64_MAX) abort();
            ++lane->processed_user_messages;
        } else if (kind == TURN_WAKE) {
            if (!active_wake || active_wake->state != WAKE_EXECUTING
                    || !lane->wake_live || s->wakes_completed == UINT64_MAX)
                abort();
            active_wake->state = WAKE_EMPTY;
            active_wake->ticket = (OresNativeWakeTicket){0};
            active_wake->originating_data_mail = 0;
            --lane->wake_live;
            ++s->wakes_completed;
        }
        /* A lane contributes at most one runnable turn regardless of how
         * many DATA/CONTROL messages it owns. */
        if (!lane_has_work(lane)) {
            if (!s->runnable_lanes) abort();
            --s->runnable_lanes;
        }
        if (typed_failure) {
            if (failure_child >= s->actor_count
                    || s->actors[failure_child].failure_state != 2) abort();
            s->actors[failure_child].failure_state = 3;
            maybe_finish_stopped_locked(s, failure_child);
        }
        if (kind == TURN_CONTROL) ++s->control_completed;
        /* Requeue at the tail if the lane still has work. No signal: this
         * carrier is about to look for its next turn itself, and the chain
         * wake at dispatch brings in another carrier for anything left. */
        rq_sync_locked(s, i);
        RQ_VALIDATE(s);
        maybe_finish_stopped_locked(s, i);
        if (!s->outstanding) {
            pthread_cond_broadcast(&s->drained);
            if (s->retired) wake_all_locked(s);
        }
        /* Keep the lock: loop straight into the next dispatch. */
    }
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_with_wakes(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    OresNativeActorPanicFn panic_callback, OresNativeActorWakeFn wake_callback,
    void* ctx) {
    const OresNativeActorSchedulerOptions options = {
        .actor_count = actors, .per_actor_capacity = capacity, .workers = workers,
        .on_message = callback, .on_control = control_callback,
        .on_panic = panic_callback, .on_wake = wake_callback, .ctx = ctx,
        .lock_protocol = ORES_NATIVE_SCHEDULER_LOCK_PRIORITY_INHERIT,
    };
    return ores_native_actor_scheduler_open_with_options(&options);
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_with_options(
    const OresNativeActorSchedulerOptions* options) {
    if (!options) return NULL;
    const size_t actors = options->actor_count;
    const size_t capacity = options->per_actor_capacity;
    const size_t workers = options->workers;
    OresNativeActorTaskFn callback = options->on_message;
    OresNativeActorTaskFn control_callback = options->on_control;
    OresNativeActorPanicFn panic_callback = options->on_panic;
    OresNativeActorWakeFn wake_callback = options->on_wake;
    OresNativeActorTaskFn task_callback = options->on_task;
    OresNativeActorTaskFn task_cancel_callback = options->on_task_cancel;
    void* ctx = options->ctx;
    const OresNativeSchedulerLockProtocol protocol = options->lock_protocol;
    if (protocol != ORES_NATIVE_SCHEDULER_LOCK_PRIORITY_INHERIT
            && protocol != ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT)
        return NULL;
    if (!!task_callback != !!task_cancel_callback) return NULL;
    if (!callback || !actors || actors > ORES_NATIVE_SCHEDULER_MAX_ACTORS
        || actors > SIZE_MAX / ORES_NATIVE_WAKE_CREDITS_PER_ACTOR
        || actors > SIZE_MAX / ORES_NATIVE_TASK_SLOTS_PER_ACTOR
        || !capacity || capacity > 65536
        || !workers || workers > MAX_CARRIERS || actors > SIZE_MAX / capacity)
        return NULL;
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    size_t initialized = 0;
    s->actors = (ActorLane*)calloc(actors, sizeof(ActorLane));
    s->slots = (uint64_t*)calloc(actors * capacity, sizeof(uint64_t));
    s->sequence_slots = (uint64_t*)calloc(actors * capacity, sizeof(uint64_t));
    if (task_callback) s->task_slots = (uint64_t*)calloc(
        actors * ORES_NATIVE_TASK_SLOTS_PER_ACTOR, sizeof(uint64_t));
    if (wake_callback) s->wake_slots = (WakeCredit*)calloc(
        actors * ORES_NATIVE_WAKE_CREDITS_PER_ACTOR, sizeof(WakeCredit));
    s->threads = (pthread_t*)calloc(workers, sizeof(pthread_t));
    s->carrier_slots = (CarrierSlot*)calloc(workers, sizeof(CarrierSlot));
    if (!s->actors || !s->slots || !s->sequence_slots
            || (task_callback && !s->task_slots)
            || (wake_callback && !s->wake_slots)
            || !s->threads || !s->carrier_slots)
        goto allocation_failed;
    for (size_t w = 0; w < workers; ++w)
        s->carrier_slots[w] = (CarrierSlot){ .scheduler = s, .index = w };
    for (int k = 0; k < TURN_CLASSES; ++k)
        s->rq_head[k] = s->rq_tail[k] = RQ_NIL;
    atomic_init(&s->work_epoch, 0);
    s->domain = allocate_scheduler_domain();
    if (!s->domain) goto allocation_failed;
    s->actor_count = actors;
    s->next_execution = 1;
    s->per_actor_capacity = capacity;
    s->worker_count = workers;
    s->callback = callback;
    s->control_callback = control_callback;
    s->panic_callback = panic_callback;
    s->wake_callback = wake_callback;
    s->task_callback = task_callback;
    s->task_cancel_callback = task_cancel_callback;
    s->context = ctx;
    for (size_t i = 0; i < actors; ++i) {
        for (int k = 0; k < TURN_CLASSES; ++k)
            s->actors[i].rq_next[k] = s->actors[i].rq_prev[k] = RQ_NIL;
        s->actors[i].failure_head = s->actors[i].failure_tail = RQ_NIL;
        s->actors[i].failure_next = RQ_NIL;
        s->actors[i].messages = s->slots + i * capacity;
        s->actors[i].mail_sequences = s->sequence_slots + i * capacity;
        s->actors[i].tasks = task_callback
            ? s->task_slots + i * ORES_NATIVE_TASK_SLOTS_PER_ACTOR : NULL;
        s->actors[i].wakes = wake_callback
            ? s->wake_slots + i * ORES_NATIVE_WAKE_CREDITS_PER_ACTOR : NULL;
        if (!ores_execution_init(&s->actors[i].authority)) goto allocation_failed;
        ++initialized;
    }
    if (!init_scheduler_mutex(s, protocol)) goto allocation_failed;
    if (pthread_cond_init(&s->ready, NULL) != 0) {
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    if (pthread_cond_init(&s->drained, NULL) != 0) {
        pthread_cond_destroy(&s->ready);
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    /* Metadata-only bootstrap. The first accepted message starts carriers. */
    return s;
allocation_failed:
    for (size_t i = 0; i < initialized; ++i)
        if (!ores_execution_destroy(&s->actors[i].authority)) abort();
    free(s->carrier_slots);
    free(s->threads);
    free(s->slots);
    free(s->sequence_slots);
    free(s->task_slots);
    free(s->wake_slots);
    free(s->actors);
    free(s);
    return NULL;
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
    CarrierSlot* slot = (CarrierSlot*)context;
    OresNativeActorScheduler* s = slot->scheduler;
    if (!s->explicit_carrier_policy && !s->cpu_affinity_configured)
        return pthread_create(tid, NULL, carrier, context);

    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) return rc;
    if (s->explicit_carrier_policy) {
        rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        if (rc == 0) rc = pthread_attr_setschedpolicy(&attr, s->carrier_policy);
        if (rc == 0) {
            struct sched_param param = {0};
            param.sched_priority = s->carrier_priority;
            rc = pthread_attr_setschedparam(&attr, &param);
        }
    }
#if defined(__linux__)
    /* Pin in the attributes, not after start: the carrier never executes an
     * instruction (or takes a turn) on a CPU outside its reservation. */
    if (rc == 0 && s->cpu_affinity_configured) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(s->cpus[slot->index % s->cpu_count], &set);
        rc = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    }
#endif
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
        if (create_carrier(&s->threads[s->started_workers],
                &s->carrier_slots[s->started_workers]) != 0)
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
/* Bootstrap-only knobs share configure_fifo's rule: no carrier and no
 * admitted work may exist yet, so carriers read them without the lock. */
static int pristine_locked(const OresNativeActorScheduler* s) {
    return !s->retired && s->started_workers == 0
        && s->accepted == 0 && s->outstanding == 0 && s->runnable_lanes == 0;
}
int ores_native_actor_scheduler_configure_idle_spin(
        OresNativeActorScheduler* s, uint32_t spin_ns) {
    if (!s || executing == s || spin_ns > MAX_IDLE_SPIN_NS) return 0;
    lock(s);
    const int ok = pristine_locked(s);
    if (ok) s->idle_spin_ns = spin_ns;
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_configure_cpu_affinity(
        OresNativeActorScheduler* s, const uint32_t* cpus, size_t count) {
    if (!s || executing == s || !cpus || !count || count > MAX_CARRIERS)
        return 0;
#if defined(__linux__)
    /* Every CPU must be one this process may already run on; asking for a
     * CPU outside the cpuset would fail at pthread_create (or worse, be
     * silently widened by a container runtime). */
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return 0;
    for (size_t i = 0; i < count; ++i)
        if (cpus[i] >= CPU_SETSIZE || !CPU_ISSET(cpus[i], &allowed)) return 0;
    lock(s);
    const int ok = pristine_locked(s);
    if (ok) {
        memcpy(s->cpus, cpus, count * sizeof(*cpus));
        s->cpu_count = count;
        s->cpu_affinity_configured = 1;
    }
    unlock(s);
    return ok;
#else
    /* No hard thread affinity on this OS: fail closed rather than pretend. */
    return 0;
#endif
}
int ores_native_actor_scheduler_cpu_affinity_observed(
        OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    int ok = s->cpu_affinity_configured
        && s->started_workers == s->worker_count;
    for (size_t w = 0; ok && w < s->started_workers; ++w)
        ok = s->carrier_slots[w].affinity_observed;
    unlock(s);
    return ok;
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
    /* Return only once every carrier is in its run loop, so the first
     * admission never races thread start-up and affinity has been observed. */
    while (ok && s->carriers_checked_in < s->started_workers)
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
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
        || s->actors[i].accepted_user_messages == UINT64_MAX
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
    /* The source-mail ordinal is minted at SUCCESSFUL enqueue, not inferred
     * from payload, carrier, or the number of completed callbacks. The pair
     * lives in parallel preallocated ring slots and cannot be reassigned by a
     * producer's Future completion or a guest-supplied token. */
    const uint64_t admitted_sequence = a->accepted_user_messages + UINT64_C(1);
    a->messages[a->tail] = payload;
    a->mail_sequences[a->tail] = admitted_sequence;
    if (++a->tail == s->per_actor_capacity) a->tail = 0;
    a->queued++;
    a->accepted_user_messages = admitted_sequence;
    s->reserved_executions++;
    s->accepted++;
    s->outstanding++;
    /* Only a lane that just became runnable needs a carrier. Mail for an
     * active or already-queued lane is picked up by whoever runs it next. */
    if (rq_sync_locked(s, i)) wake_one_locked(s);
    RQ_VALIDATE(s);
    return 1;
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
    wake_all_locked(s);
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
static OresNativeSendStatus try_send_impl(
    OresNativeActorScheduler* s, OresNativeActorRef actor, uint64_t payload,
    uint64_t* out_sequence) {
    if (out_sequence) *out_sequence = 0;
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
        } else if (lane->accepted_user_messages == UINT64_MAX
                || lane->queued + lane->active >= s->per_actor_capacity) {
            status = ORES_NATIVE_SEND_MAILBOX_FULL;
        } else if (s->accepted >= UINT64_MAX - s->reserved_wakes
                || !execution_slot_available_locked(s)) {
            status = ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
        } else {
            /* enqueue_locked and stop() arbitrate under this same lock.
             * A successful admission precedes stop, even if processing later
             * fails; a stop that wins first is always rejected. */
            if (enqueue_locked(s, (size_t)actor.actor_slot, payload)) {
                status = ORES_NATIVE_SEND_ACCEPTED;
                /* Minted by enqueue_locked under this same scheduler lock;
                 * no callback can complete before the transport binds its
                 * descriptor to this accepted mail sequence. */
                if (out_sequence)
                    *out_sequence = lane->accepted_user_messages;
            } else {
                /* enqueue_locked already counted this rejected admission. */
                counted_in_enqueue = 1;
                status = ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
            }
        }
    }
    if (status != ORES_NATIVE_SEND_ACCEPTED && !counted_in_enqueue
        && s->rejected != UINT64_MAX)
        s->rejected++;
    unlock(s);
    return status;
}
OresNativeTaskAdmissionStatus ores_native_actor_scheduler_try_schedule_task(
        OresNativeActorScheduler* s, OresNativeActorRef actor, uint64_t task_handle) {
    if (!s || !task_handle || !actor.scheduler_domain)
        return ORES_NATIVE_TASK_INVALID_ACTOR;
    if (!s->task_callback || !s->task_cancel_callback)
        return ORES_NATIVE_TASK_NOT_CONFIGURED;
    /* Verify the physical single-writer lease BEFORE taking scheduler lock:
     * authority inspection itself uses the lane's independent phase mutex.
     * This admits nested TASK and authenticated WAKE turns, never CONTROL
     * or a foreign Future, reactor, root Task or another actor. */
    if (executing_kind == TURN_CONTROL
            || (executing_kind != TURN_DATA && executing_kind != TURN_TASK
                    && executing_kind != TURN_WAKE)
            || !ores_native_actor_scheduler_is_current_actor(s, actor))
        return ORES_NATIVE_TASK_FOREIGN_CARRIER;

    lock(s);
    OresNativeTaskAdmissionStatus status;
    if (!matching(s, actor)) {
        status = ORES_NATIVE_TASK_INVALID_ACTOR;
    } else {
        ActorLane* lane = &s->actors[actor.actor_slot];
        if (s->retired || lane->retired) {
            status = ORES_NATIVE_TASK_STOPPED;
        } else if (lane->task_queued >= ORES_NATIVE_TASK_SLOTS_PER_ACTOR) {
            status = ORES_NATIVE_TASK_MAILBOX_FULL;
        } else if (s->accepted >= UINT64_MAX - s->reserved_wakes
                || s->outstanding == UINT64_MAX
                || !execution_slot_available_locked(s)) {
            status = ORES_NATIVE_TASK_RESOURCE_EXHAUSTED;
        } else {
            /* Caller currently owns the actor lane, so its scheduler already
             * has a live carrier; no new pthread, supervisor worker, or
             * inline callback is ever introduced by task admission. */
            lane->tasks[lane->task_tail] = task_handle;
            if (++lane->task_tail == ORES_NATIVE_TASK_SLOTS_PER_ACTOR)
                lane->task_tail = 0;
            ++lane->task_queued;
            ++s->reserved_executions;
            ++s->outstanding;
            ++s->accepted;
            (void)rq_sync_locked(s, (size_t)actor.actor_slot);
            RQ_VALIDATE(s);
            status = ORES_NATIVE_TASK_ACCEPTED;
        }
    }
    unlock(s);
    return status;
}

OresNativeSendStatus ores_native_actor_scheduler_try_send(
    OresNativeActorScheduler* s, OresNativeActorRef actor, uint64_t payload) {
    return try_send_impl(s, actor, payload, NULL);
}
OresNativeSendStatus ores_native_actor_scheduler_try_send_receipted(
    OresNativeActorScheduler* s, OresNativeActorRef actor, uint64_t payload,
    uint64_t* out_sequence) {
    if (!out_sequence) return ORES_NATIVE_SEND_INVALID_REF;
    return try_send_impl(s, actor, payload, out_sequence);
}
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
     * called with a scheduler opened through the legacy no-wake constructors.
     * Such schedulers deliberately have no wake table; fail closed instead of
     * dereferencing a NULL per-lane wake pointer. */
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
            || old->descriptor_generation != t.descriptor_generation)
        return NULL;
    return w;
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
    int ok = !s->retired && s->wake_callback && matching(s, producer)
        && (target.scheduler_domain != s->domain
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
        /* Guarantee worker availability up front, including for an actor
         * that has not yet received ordinary USER or CONTROL work.
         * This intentionally moves potentially failing pthread_create into
         * RESERVE, not the irrevocable transfer/publish path. */
        ok = w && ensure_workers_locked(s, s->worker_count);
    }
    if (ok) {
        ++w->generation;
        /* Scheduler->authority lock order. A producer thread, CONTROL or
         * WAKE callback cannot forge the original DATA-mail provenance. */
        w->originating_data_mail =
            executing == s && executing_data_mail_sequence
            && executing_actor.scheduler_domain == producer.scheduler_domain
            && executing_actor.actor_slot == producer.actor_slot
            && executing_actor.generation == producer.generation
            && ores_execution_owns_running(&lane->authority, executing_claim)
            ? executing_data_mail_sequence : 0;
        const size_t slot = (size_t)(w - lane->wakes);
        w->ticket = (OresNativeWakeTicket){
            .producer = producer, .target = target,
            .slot = slot, .slot_generation = w->generation,
            .continuation_slot = continuation_slot,
            .continuation_generation = continuation_generation,
            .code_generation = code_generation,
            .descriptor_generation = descriptor_generation
        };
        w->state = WAKE_RESERVED;
        ++s->reserved_wakes;
        ++lane->wake_live;
        *out = w->ticket;
    }
    unlock(s);
    return ok;
}
/* Verify a WAKE ticket was genuinely RESERVED during THIS authenticated
 * originating DATA-mail turn, not constructed from matching actor IDs or a
 * guest-supplied original-mail integer. No wake-ticket ABI change required:
 * the proof is in the scheduler-owned WakeCredit slot. */
int ores_native_actor_scheduler_current_data_issued_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket,
        uint64_t expected_original_mail) {
    if (!s || !expected_original_mail) return 0;
    uint64_t authenticated_mail = 0;
    if (!ores_native_actor_scheduler_current_data_mail_sequence(
            s, ticket.producer, &authenticated_mail)
            || authenticated_mail != expected_original_mail)
        return 0;
    lock(s);
    WakeCredit* w = matching_wake_locked(s, ticket);
    const int ok = w && w->state == WAKE_RESERVED
        && w->originating_data_mail == expected_original_mail
        && ores_execution_owns_running(
            &s->actors[ticket.producer.actor_slot].authority, executing_claim);
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
    if (rq_sync_locked(s, (size_t)ticket.producer.actor_slot)) wake_one_locked(s);
    RQ_VALIDATE(s);
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
            || executing_wake.target.scheduler_domain != ticket.target.scheduler_domain
            || executing_wake.target.actor_slot != ticket.target.actor_slot
            || executing_wake.target.generation != ticket.target.generation)
        return 0;
    return ores_native_actor_scheduler_is_current_actor(s, ticket.producer);
}
/* Request-local SourceTask resume may access its original scope only on the
 * exact scheduler-issued continuation WAKE, with a successful READY outcome,
 * and only while its producer actor remains RUNNING on this physical carrier.
 * CANCELLED/FAILED, foreign callbacks and STOPPING cleanup fail closed. */
int ores_native_actor_scheduler_is_current_ready_wake(
        OresNativeActorScheduler* s, OresNativeWakeTicket ticket) {
    if (!ores_native_actor_scheduler_is_current_wake(s, ticket)
            || executing_wake_outcome != ORES_NATIVE_WAKE_READY)
        return 0;
    return ores_execution_owns_running(
        &s->actors[ticket.producer.actor_slot].authority, executing_claim);
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
        if (rq_sync_locked(s, (size_t)parent.actor_slot)) wake_one_locked(s);
        RQ_VALIDATE(s);
    } else if (s->rejected != UINT64_MAX) ++s->rejected;
    unlock(s);
    return ok;
}
/* Trusted host reservation before the child's first USER admission. */
int ores_native_actor_scheduler_bind_supervisor(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorRef child) {
    if (!s) return 0;
    lock(s);
    int ok = !s->retired && s->panic_callback
        && matching(s, parent) && matching(s, child)
        && parent.actor_slot != child.actor_slot
        && !s->actors[parent.actor_slot].retired
        && !s->actors[child.actor_slot].retired
        && !s->actors[child.actor_slot].supervisor_bound
        && !s->actors[child.actor_slot].accepted_user_messages;
    if (ok) {
        OresNativeActorRef cursor = parent;
        for (size_t depth = 0; depth < s->actor_count; ++depth) {
            ActorLane* ancestor = &s->actors[cursor.actor_slot];
            if (!ancestor->supervisor_bound) break;
            cursor = ancestor->supervisor;
            if (!matching(s, cursor) || cursor.actor_slot == child.actor_slot) {
                ok = 0;
                break;
            }
            if (depth + 1 == s->actor_count) ok = 0;
        }
    }
    if (ok) {
        ActorLane* slot = &s->actors[child.actor_slot];
        slot->supervisor = parent;
        slot->supervisor_bound = 1;
    }
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_publish_failure(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorTerminalFailure failure) {
    if (!s) return 0;
    lock(s);
    int ok = s->panic_callback && !s->retired
        && matching(s, parent) && matching(s, failure.child)
        && parent.actor_slot != failure.child.actor_slot
        && failure.kind >= ORES_NATIVE_CONTROL_PANIC
        && failure.kind <= ORES_NATIVE_CONTROL_STOPPED
        && !s->actors[parent.actor_slot].retired
        && s->actors[failure.child.actor_slot].supervisor_bound
        && s->actors[failure.child.actor_slot].supervisor.scheduler_domain == parent.scheduler_domain
        && s->actors[failure.child.actor_slot].supervisor.actor_slot == parent.actor_slot
        && s->actors[failure.child.actor_slot].supervisor.generation == parent.generation
        && s->actors[failure.child.actor_slot].failure_state == 0
        && s->accepted < UINT64_MAX - s->reserved_wakes
        && execution_slot_available_locked(s);
    if (ok) {
        ActorLane* owner = &s->actors[parent.actor_slot];
        ActorLane* child = &s->actors[failure.child.actor_slot];
        const int new_lane = !owner->active && !lane_has_work(owner);
        if (!ensure_workers_locked(s, s->runnable_lanes + (size_t)new_lane)) {
            if (s->rejected != UINT64_MAX) ++s->rejected;
            unlock(s);
            return 0;
        }
        if (new_lane) ++s->runnable_lanes;
        child->failure = failure;
        child->failure_state = 1;
        child->failure_next = RQ_NIL;
        if (owner->failure_tail == RQ_NIL)
            owner->failure_head = (uint32_t)failure.child.actor_slot;
        else
            s->actors[owner->failure_tail].failure_next = (uint32_t)failure.child.actor_slot;
        owner->failure_tail = (uint32_t)failure.child.actor_slot;
        ++s->reserved_executions;
        ++s->accepted;
        ++s->outstanding;
        if (rq_sync_locked(s, (size_t)parent.actor_slot)) wake_one_locked(s);
        RQ_VALIDATE(s);
    } else if (s->rejected != UINT64_MAX) ++s->rejected;
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
        if (!lane->active && !lane->has_control && !lane->wake_ready
                && !lane->task_queued) {
            if (!s->runnable_lanes) abort();
            --s->runnable_lanes;
        }

        lane->head = lane->tail;
        lane->queued = 0;
        lane->abandoned_user_messages += (uint64_t)abandoned;
        s->reserved_executions -= abandoned;
        s->outstanding -= abandoned;
        rq_sync_locked(s, (size_t)actor.actor_slot); /* leaves the DATA queue */
        RQ_VALIDATE(s);
    }

    if (!ores_execution_stop(&lane->authority, lane->generation)) abort();
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
int ores_native_actor_scheduler_retain_resource(
        OresNativeActorScheduler* s, OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int ok = matching(s, actor) && !s->retired
        && !s->actors[actor.actor_slot].retired
        && s->actors[actor.actor_slot].resource_loans != UINT64_MAX
        && s->resource_loans != UINT64_MAX;
    if (ok) {
        ++s->actors[actor.actor_slot].resource_loans;
        ++s->resource_loans;
    }
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_release_resource(
        OresNativeActorScheduler* s, OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int ok = matching(s, actor)
        && s->actors[actor.actor_slot].resource_loans != 0
        && s->resource_loans != 0;
    if (ok) {
        --s->actors[actor.actor_slot].resource_loans;
        --s->resource_loans;
        maybe_finish_stopped_locked(s, (size_t)actor.actor_slot);
        pthread_cond_broadcast(&s->drained);
    }
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
    if (!s || executing == s) return 0;
    lock(s);
    if (!matching(s, actor) || !s->actors[actor.actor_slot].retired) {
        unlock(s);
        return 0;
    }
    ActorLane* lane = &s->actors[actor.actor_slot];
    while (lane_has_work(lane) || lane->active || lane->wake_live
        || lane->resource_loans
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
            if (ores_execution_phase(&lane->authority) != ORES_EXEC_STOPPING
                && ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED)
                if (!ores_execution_stop(&lane->authority, lane->generation)) abort();
            maybe_finish_stopped_locked(s, i);
        }
    }
    wake_all_locked(s);
    unlock(s);
}
int ores_native_actor_scheduler_is_current_carrier(OresNativeActorScheduler* s) {
    return s && executing == s;
}
/* A pair of native actor generations must remain live across the entire
 * bounded host commit. Ordinary STOP, sealed STOP and global retirement
 * all arbitrate using this same lifecycle lock. The callback is restricted
 * to allocation-free bookkeeping and must never reenter scheduler APIs. */
int ores_native_actor_scheduler_commit_if_both_live_v1(
        OresNativeActorScheduler* s, OresNativeActorRef sender,
        OresNativeActorRef recipient, OresNativeActorBoundedCommitV1 commit,
        void* opaque) {
    if (!s || !commit) return 0;
    lock(s);
    int live = !s->retired && matching(s, sender) && matching(s, recipient);
    if (live) {
        ActorLane* from = &s->actors[sender.actor_slot];
        ActorLane* to = &s->actors[recipient.actor_slot];
        live = !from->retired && !to->retired;
        if (live) {
            const OresExecutionPhase from_phase =
                ores_execution_phase(&from->authority);
            const OresExecutionPhase to_phase =
                ores_execution_phase(&to->authority);
            live = (from_phase == ORES_EXEC_READY ||
                    from_phase == ORES_EXEC_RUNNING ||
                    from_phase == ORES_EXEC_SUSPENDED)
                && (to_phase == ORES_EXEC_READY ||
                    to_phase == ORES_EXEC_RUNNING ||
                    to_phase == ORES_EXEC_SUSPENDED);
        }
    }
    const int accepted = live ? (commit(opaque) != 0) : 0;
    unlock(s);
    return accepted;
}
int ores_native_actor_scheduler_in_any_actor_turn(void) {
    /* executing is installed only during a scheduler-owned callback and
     * cleared before leaving it, including CONTROL and WAKE callbacks.
     * A synthetic SourceTask worker is not exempt from this fence. */
    return executing != NULL
        && executing_actor.scheduler_domain != 0
        && executing_claim.carrier != 0;
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
/* Original user DATA-mail sequence is available only while the authenticated
 * actor callback is RUNNING on this scheduler's physical carrier. It is not
 * available to CONTROL, WAKE, detached Tasks or external Future producers.
 * The sequence was minted at mailbox admission, not recovered from payload. */
int ores_native_actor_scheduler_current_data_mail_sequence(
        OresNativeActorScheduler* s, OresNativeActorRef actor,
        uint64_t* out_sequence) {
    if (!s || !out_sequence || executing != s
            || !executing_data_mail_sequence || !actor.scheduler_domain
            || actor.scheduler_domain != executing_actor.scheduler_domain
            || actor.actor_slot != executing_actor.actor_slot
            || actor.generation != executing_actor.generation
            || !executing_claim.carrier)
        return 0;
    if (!ores_execution_owns_running(
            &s->actors[actor.actor_slot].authority, executing_claim))
        return 0;
    *out_sequence = executing_data_mail_sequence;
    return 1;
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
    if (ok) wake_all_locked(s);
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* s) {
    if (!s || executing == s) return 0;
    lock(s);
    while (s->outstanding || s->resource_loans)
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    unlock(s);
    return 1;
}
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* s) {
    if (!s || executing == s) return 0;
    lock(s);
    int ready = s->retired && !s->outstanding && !s->reserved_wakes
        && !s->resource_loans;
    if (ready) {
        for (size_t i = 0; i < s->actor_count; ++i) {
            ActorLane* lane = &s->actors[i];
            if (lane->spawned
                && (ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED
                    || !lane->terminal_valid)) {
                ready = 0;
                break;
            }
        }
    }
    if (ready) wake_all_locked(s);
    unlock(s);
    if (!ready) return 0;
    /* Caller must externally fence producers; no concurrent API calls now. */
    for (size_t i = 0; i < s->started_workers; ++i)
        if (pthread_join(s->threads[i], NULL) != 0) abort();
    if (pthread_cond_destroy(&s->drained) != 0
        || pthread_cond_destroy(&s->ready) != 0
        || pthread_mutex_destroy(&s->lock) != 0) abort();
    for (size_t i = 0; i < s->actor_count; ++i)
        if (!ores_execution_destroy(&s->actors[i].authority)) abort();
    free(s->carrier_slots);
    free(s->threads);
    free(s->slots);
    free(s->sequence_slots);
    free(s->task_slots);
    free(s->wake_slots);
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
