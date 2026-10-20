#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeExecutionAuthority.h"
#include "oreslang/runtime/NativePhysicalTurnGate.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>
#ifdef ORESLANG_NATIVE_TEST_PTHREAD_CREATE_FAILURE
#include <errno.h>
/* Only compiled in the isolated fault-injection CTest; absent in production. */
static _Atomic int fail_next_carrier_create;
void ores_native_actor_scheduler_test_fail_next_create(void) {
    atomic_store_explicit(&fail_next_carrier_create, 1, memory_order_relaxed);
}
#endif
static int create_carrier(pthread_t* tid, void* context);

typedef struct {
    uint64_t* messages; /* fixed ring, assigned at scheduler startup */
    size_t head, tail, queued;
    unsigned active;
    unsigned has_control;
    uint64_t control_payload;
    OresNativeActorTerminalFailure failure;
    unsigned control_is_failure;
    unsigned spawned, retired;
    uint64_t generation;
    OresNativeExecutionAuthority authority;
} ActorLane;
struct OresNativeActorScheduler {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t drained;
    ActorLane* actors;
    uint64_t* slots;
    pthread_t* threads;
    size_t actor_count, per_actor_capacity, worker_count, started_workers;
    size_t next_actor, outstanding, runnable_lanes;
    uint64_t domain;
    /* Mutex-owned occupancy indexes, never actor ownership or authority. */
    uint64_t* ready_control_bits;
    uint64_t* ready_data_bits;
    size_t ready_word_count;
    uint64_t accepted, rejected, completed, control_completed;
    int retired;
    OresNativeActorTaskFn callback;
    OresNativeActorTaskFn control_callback;
    OresNativeActorPanicFn panic_callback;
    void* context;
    OresNativePhysicalTurnGate* physical_turn_gate;
};
static _Thread_local OresNativeActorScheduler* executing;
static _Thread_local OresNativeActorRef executing_actor;
static _Thread_local OresExecutionClaim executing_claim;
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

static void lock(OresNativeActorScheduler* s) {
    if (pthread_mutex_lock(&s->lock) != 0) abort();
}
static void unlock(OresNativeActorScheduler* s) {
    if (pthread_mutex_unlock(&s->lock) != 0) abort();
}
/* Mutex-owned CONTROL/DATA ready indexes. All mutators update these
 * atomically with the corresponding mailbox and actor execution lease. */
enum { ORES_READY_WORD_BITS = 64, ORES_READY_MAX_ACTORS = 65536 };
enum { ORES_LARGE_POOL_MAX_MAILBOX_CELLS = 1048576 };

static void refresh_ready_locked(OresNativeActorScheduler* s, size_t i) {
    if (i >= s->actor_count) abort();
    ActorLane* lane = &s->actors[i];
    const size_t word = i / ORES_READY_WORD_BITS;
    if (word >= s->ready_word_count) abort();
    const uint64_t bit = UINT64_C(1) << (i % ORES_READY_WORD_BITS);
    if (!lane->active && lane->has_control)
        s->ready_control_bits[word] |= bit;
    else
        s->ready_control_bits[word] &= ~bit;
    if (!lane->active && lane->queued)
        s->ready_data_bits[word] |= bit;
    else
        s->ready_data_bits[word] &= ~bit;
}

/* Search only populated words with exactly the rotating slot order used
 * before indexing. Never mutate the cursor speculatively: physical permits
 * may deny admission after selection, and must not reorder queued mail. */
static size_t select_ready_locked(const OresNativeActorScheduler* s,
                                  const uint64_t* index,
                                  size_t start, int control) {
    for (int wrap = 0; wrap < 2; ++wrap) {
        const size_t begin = wrap ? 0 : start;
        const size_t end = wrap ? start : s->actor_count;
        if (begin >= end) continue;
        const size_t first = begin / ORES_READY_WORD_BITS;
        const size_t last = (end - 1) / ORES_READY_WORD_BITS;
        for (size_t word = first; word <= last; ++word) {
            uint64_t bits = index[word];
            if (word == first)
                bits &= UINT64_MAX << (begin % ORES_READY_WORD_BITS);
            if (word == last)
                bits &= UINT64_MAX >> (63 - ((end - 1) % ORES_READY_WORD_BITS));
            if (!bits) continue;
            const unsigned bit = (unsigned)__builtin_ctzll(bits);
            const size_t slot = word * ORES_READY_WORD_BITS + bit;
            const ActorLane* lane = &s->actors[slot];
            if (lane->active || !(control ? lane->has_control : lane->queued))
                abort(); /* Stale occupancy index is a correctness failure. */
            return slot;
        }
    }
    return SIZE_MAX;
}

static int find_ready(OresNativeActorScheduler* s, size_t* actor, int* control,
        size_t recent_actor, unsigned consecutive) {
    /* CONTROL always takes precedence, including over cache-warm DATA. */
    size_t i = select_ready_locked(s, s->ready_control_bits, s->next_actor, 1);
    if (i != SIZE_MAX) {
        *actor = i;
        *control = 1;
        return 1;
    }
    /* Preserve previous at-most-two consecutive same-carrier DATA turns.
     * This is not a binding guarantee or authority to borrow actor state. */
    if (recent_actor < s->actor_count && consecutive < 2) {
        const uint64_t bit = UINT64_C(1) << (recent_actor % ORES_READY_WORD_BITS);
        if ((s->ready_data_bits[recent_actor / ORES_READY_WORD_BITS] & bit) != 0) {
            *actor = recent_actor;
            *control = 0;
            return 1;
        }
    }
    i = select_ready_locked(s, s->ready_data_bits, s->next_actor, 0);
    if (i == SIZE_MAX) return 0;
    *actor = i;
    *control = 0;
    return 1;
}
static void* carrier(void* raw) {
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)raw;
    size_t recent_actor = SIZE_MAX;
    unsigned consecutive = 0;
    for (;;) {
        lock(s);
        size_t i = 0;
        int control = 0;
        int gate_held = 0;
        for (;;) {
            int found = find_ready(s, &i, &control, recent_actor, consecutive);
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
        /* Only an acquired physical turn can advance fairness order. */
        s->next_actor = (i + 1) % s->actor_count;
        consecutive = (i == recent_actor && !control)
                ? (consecutive < 2 ? consecutive + 1 : 2) : 1;
        recent_actor = i;
        ActorLane* lane = &s->actors[i];
        uint64_t payload;
        OresNativeActorTerminalFailure failure = {0};
        int typed_failure = control && lane->control_is_failure;
        if (control) {
            payload = lane->control_payload;
            if (typed_failure) failure = lane->failure;
            lane->control_is_failure = 0;
            lane->has_control = 0;
        } else {
            payload = lane->messages[lane->head];
            lane->head = (lane->head + 1) % s->per_actor_capacity;
            lane->queued--;
        }
        OresExecutionClaim execution_claim = {0};
        /* Only this scheduler can certify that the dequeued envelope was
         * admitted before retirement. Never time out an active guest owner. */
        if (!ores_execution_enter(&lane->authority, lane->generation,
                (uint64_t)(uintptr_t)&carrier_identity, 1, &execution_claim))
            abort();
        lane->active = 1;
        refresh_ready_locked(s, i);
        unlock(s);

        // Actor identity is authorized only by the physical execution claim.
        executing_actor = (OresNativeActorRef){s->domain, (uint64_t)i, lane->generation};
        executing_claim = execution_claim;
        executing = s;
        if (typed_failure) s->panic_callback(s->context, i, failure);
        else if (control) s->control_callback(s->context, i, payload);
        else s->callback(s->context, i, payload);
        executing_actor = (OresNativeActorRef){0, 0, 0};
        executing_claim = (OresExecutionClaim){0};
        executing = NULL;

        lock(s);
        if (!lane->active || !s->outstanding || s->completed == UINT64_MAX) abort();
        /* Leave the generation-checked guest execution claim first, but
         * keep lane.active/outstanding until the gate has completed its
         * cross-domain wakeup. Otherwise drain() may report quiescence and
         * the VM may destroy a scheduler while a gate-release callback is
         * still trying to signal that scheduler. */
        if (!ores_execution_leave(&lane->authority, execution_claim, 0)) abort();
        unlock(s);
        if (gate_held) ores_native_turn_gate_release(s->physical_turn_gate);
        lock(s);
        if (!lane->active || !s->outstanding || s->completed == UINT64_MAX) abort();
        lane->active = 0;
        refresh_ready_locked(s, i);
        s->outstanding--;
        s->completed++;
        /* A lane contributes at most one runnable turn regardless of how
         * many DATA/CONTROL messages it owns. */
        if (!lane->queued && !lane->has_control) {
            if (!s->runnable_lanes) abort();
            --s->runnable_lanes;
        }
        if (control) ++s->control_completed;
        if (lane->queued || lane->has_control) pthread_cond_signal(&s->ready);
        if (lane->retired && !lane->active && !lane->queued && !lane->has_control) {
            if (ores_execution_phase(&lane->authority) == ORES_EXEC_STOPPING
                && !ores_execution_retired(&lane->authority, lane->generation))
                abort();
            pthread_cond_broadcast(&s->drained);
        }
        if (!s->outstanding) {
            pthread_cond_broadcast(&s->drained);
            if (s->retired) pthread_cond_broadcast(&s->ready);
        }
        unlock(s);
    }
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_typed_control(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    OresNativeActorPanicFn panic_callback, void* ctx) {
    if (!callback || !actors || actors > ORES_READY_MAX_ACTORS
        || !capacity || capacity > 65536 || !workers || workers > 128
        || actors > SIZE_MAX / capacity
        || (actors > 1024 && actors * capacity > ORES_LARGE_POOL_MAX_MAILBOX_CELLS))
        return NULL;
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    size_t initialized = 0;
    s->actors = (ActorLane*)calloc(actors, sizeof(ActorLane));
    s->slots = (uint64_t*)calloc(actors * capacity, sizeof(uint64_t));
    s->threads = (pthread_t*)calloc(workers, sizeof(pthread_t));
    s->ready_word_count = (actors + ORES_READY_WORD_BITS - 1)
        / ORES_READY_WORD_BITS;
    s->ready_control_bits = (uint64_t*)calloc(s->ready_word_count, sizeof(uint64_t));
    s->ready_data_bits = (uint64_t*)calloc(s->ready_word_count, sizeof(uint64_t));
    if (!s->actors || !s->slots || !s->threads
        || !s->ready_control_bits || !s->ready_data_bits) goto allocation_failed;
    s->domain = allocate_scheduler_domain();
    if (!s->domain) goto allocation_failed;
    s->actor_count = actors;
    s->per_actor_capacity = capacity;
    s->worker_count = workers;
    s->callback = callback;
    s->control_callback = control_callback;
    s->panic_callback = panic_callback;
    s->context = ctx;
    for (size_t i = 0; i < actors; ++i) {
        s->actors[i].messages = s->slots + i * capacity;
        if (!ores_execution_init(&s->actors[i].authority)) goto allocation_failed;
        ++initialized;
    }
    if (pthread_mutex_init(&s->lock, NULL) != 0) goto allocation_failed;
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
    free(s->ready_data_bits);
    free(s->ready_control_bits);
    free(s->threads);
    free(s->slots);
    free(s->actors);
    free(s);
    return NULL;
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
    return pthread_create(tid, NULL, carrier, context);
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
static int enqueue_locked(OresNativeActorScheduler* s, size_t i, uint64_t payload) {
    if (s->retired || i >= s->actor_count
        || !s->actors[i].spawned || s->actors[i].retired
        || s->actors[i].queued + s->actors[i].active >= s->per_actor_capacity
        || s->accepted == UINT64_MAX) {
        if (s->rejected != UINT64_MAX) s->rejected++;
        return 0;
    }
    ActorLane* a = &s->actors[i];
    const int new_lane = !a->active && !a->queued && !a->has_control;
    if (!ensure_workers_locked(s, s->runnable_lanes + (size_t)new_lane)) {
        if (s->rejected != UINT64_MAX) ++s->rejected;
        return 0;
    }
    if (new_lane) ++s->runnable_lanes;
    a->messages[a->tail] = payload;
    a->tail = (a->tail + 1) % s->per_actor_capacity;
    a->queued++;
    refresh_ready_locked(s, i);
    s->accepted++;
    s->outstanding++;
    pthread_cond_signal(&s->ready);
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
        } else if (lane->queued + lane->active >= s->per_actor_capacity) {
            status = ORES_NATIVE_SEND_MAILBOX_FULL;
        } else if (s->accepted == UINT64_MAX) {
            status = ORES_NATIVE_SEND_RESOURCE_EXHAUSTED;
        } else {
            /* enqueue_locked and stop() arbitrate under this same lock.
             * A successful admission precedes stop, even if processing later
             * fails; a stop that wins first is always rejected. */
            if (enqueue_locked(s, (size_t)actor.actor_slot, payload)) {
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
        s->rejected++;
    unlock(s);
    return status;
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
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* s, OresNativeActorRef parent, uint64_t payload) {
    if (!s) return 0;
    lock(s);
    int ok = !s->retired && s->control_callback && matching(s, parent)
        && !s->actors[parent.actor_slot].retired
        && !s->actors[parent.actor_slot].has_control
        && s->accepted != UINT64_MAX;
    if (ok) {
        ActorLane* lane = &s->actors[parent.actor_slot];
        const int new_lane = !lane->active && !lane->queued;
        if (!ensure_workers_locked(s, s->runnable_lanes + (size_t)new_lane)) {
            if (s->rejected != UINT64_MAX) ++s->rejected;
            unlock(s);
            return 0;
        }
        if (new_lane) ++s->runnable_lanes;
        lane->control_payload = payload;
        lane->control_is_failure = 0;
        lane->has_control = 1;
        refresh_ready_locked(s, (size_t)parent.actor_slot);
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
        && s->accepted != UINT64_MAX;
    if (ok) {
        ActorLane* a = &s->actors[parent.actor_slot];
        const int new_lane = !a->active && !a->queued;
        if (!ensure_workers_locked(s, s->runnable_lanes + (size_t)new_lane)) {
            if (s->rejected != UINT64_MAX) ++s->rejected;
            unlock(s);
            return 0;
        }
        if (new_lane) ++s->runnable_lanes;
        a->failure = failure;
        a->control_is_failure = 1;
        a->has_control = 1;
        refresh_ready_locked(s, (size_t)parent.actor_slot);
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
int ores_native_actor_scheduler_stop(OresNativeActorScheduler* s,
                                     OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    if (!matching(s, actor) || s->actors[actor.actor_slot].retired) {
        unlock(s); return 0;
    }
    ActorLane* lane = &s->actors[actor.actor_slot];
    lane->retired = 1;
    if (!ores_execution_stop(&lane->authority, lane->generation)) abort();
    if (!lane->active && !lane->queued && !lane->has_control)
        if (!ores_execution_retired(&lane->authority, lane->generation)) abort();
    unlock(s);
    return 1;
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
    int done = matching(s, actor) && s->actors[actor.actor_slot].retired
        && !s->actors[actor.actor_slot].queued && !s->actors[actor.actor_slot].has_control
        && !s->actors[actor.actor_slot].active
        && ores_execution_phase(&s->actors[actor.actor_slot].authority)
            == ORES_EXEC_TERMINATED;
    unlock(s);
    return done;
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
    while (lane->queued || lane->has_control || lane->active
        || ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED) {
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
        lane->retired = 1;
        if (lane->spawned) {
            if (ores_execution_phase(&lane->authority) != ORES_EXEC_STOPPING
                && ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED)
                if (!ores_execution_stop(&lane->authority, lane->generation)) abort();
            if (!lane->active && !lane->queued && !lane->has_control
                && ores_execution_phase(&lane->authority) == ORES_EXEC_STOPPING)
                if (!ores_execution_retired(&lane->authority, lane->generation)) abort();
        }
    }
    pthread_cond_broadcast(&s->ready);
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
int ores_native_actor_scheduler_signal_control(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    int ok = !s->retired;
    if (ok && pthread_cond_broadcast(&s->ready) != 0) abort();
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* s) {
    if (!s || executing == s) return 0;
    lock(s);
    while (s->outstanding)
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    unlock(s);
    return 1;
}
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* s) {
    if (!s || executing == s) return 0;
    lock(s);
    int ready = s->retired && !s->outstanding;
    if (ready) {
        for (size_t i = 0; i < s->actor_count; ++i) {
            ActorLane* lane = &s->actors[i];
            if (lane->spawned
                && ores_execution_phase(&lane->authority) != ORES_EXEC_TERMINATED) {
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
    if (pthread_cond_destroy(&s->drained) != 0
        || pthread_cond_destroy(&s->ready) != 0
        || pthread_mutex_destroy(&s->lock) != 0) abort();
    for (size_t i = 0; i < s->actor_count; ++i)
        if (!ores_execution_destroy(&s->actors[i].authority)) abort();
    free(s->ready_data_bits);
    free(s->ready_control_bits);
    free(s->threads);
    free(s->slots);
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
