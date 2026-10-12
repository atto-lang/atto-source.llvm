#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>

typedef struct {
    uint64_t* messages; /* fixed ring, assigned at scheduler startup */
    size_t head, tail, queued;
    unsigned active;
    unsigned has_control;
    uint64_t control_payload;
    OresNativeActorTerminalFailure failure;
    unsigned control_is_failure;
    unsigned spawned, retired;
    unsigned published; /* hidden until a host RESTART transaction commits */
    uint64_t restart_lease_nonce;
    pthread_t restart_lease_owner;
    /* Mutually exclusive terminal classification, under the scheduler lock. */
    unsigned failure_attempted, failure_published, normal_exit_sealed;
    /* Exact failed-child generation retains a typed terminal envelope
     * even if its original supervisor CONTROL admission fails. */
    OresNativeActorRef terminal_parent;
    OresNativeActorTerminalFailure terminal_failure;
    uint64_t terminal_claim_nonce;
    unsigned terminal_receipt_state;
    unsigned terminal_was_published;
    uint64_t generation;
} ActorLane;
struct OresNativeActorScheduler {
    pthread_mutex_t lock;
    /* A separate host publication gate: never hold lock across registry
     * calls. STOP/retire cannot invalidate a leased physical incarnation. */
    pthread_mutex_t restart_publication_gate;
    uint64_t next_restart_lease_nonce;
    uint64_t next_terminal_claim_nonce;
    pthread_cond_t ready;
    pthread_cond_t drained;
    ActorLane* actors;
    uint64_t* slots;
    pthread_t* threads;
    size_t actor_count, per_actor_capacity, worker_count;
    size_t next_actor, outstanding;
    uint64_t domain;
    uint64_t accepted, rejected, completed, control_completed;
    int retired;
    OresNativeActorTaskFn callback;
    OresNativeActorTaskFn control_callback;
    OresNativeActorPanicFn panic_callback;
    OresNativeActorIdentityTaskFn identity_callback;
    OresNativeActorIdentityTaskFn identity_control_callback;
    OresNativeActorIdentityPanicFn identity_panic_callback;
    unsigned identity_callbacks;
    void* context;
};
static _Thread_local OresNativeActorScheduler* executing;
/* Only the acquiring host thread may release a restart lease. In particular,
 * it must not synchronously STOP/retire the same scheduler: pthread normal
 * mutexes are not recursive and would deadlock on that reentry. */
static _Thread_local OresNativeActorScheduler* owned_restart_lease;
static _Atomic(uint64_t) next_scheduler_domain = 1;

static void lock(OresNativeActorScheduler* s) {
    if (pthread_mutex_lock(&s->lock) != 0) abort();
}
static void unlock(OresNativeActorScheduler* s) {
    if (pthread_mutex_unlock(&s->lock) != 0) abort();
}
static int find_ready(OresNativeActorScheduler* s, size_t* actor, int* control) {
    /* CONTROL must be admitted ahead of normal mail, across all actors.
     * The parent still owns its exclusive logical lane. */
    for (int pass = 1; pass >= 0; --pass) {
        for (size_t offset = 0; offset < s->actor_count; ++offset) {
            size_t i = (s->next_actor + offset) % s->actor_count;
            ActorLane* candidate = &s->actors[i];
            if (candidate->active) continue;
            if (pass) {
                if (!candidate->has_control) continue;
                if (candidate->control_is_failure) {
                    /* A stopped child may have published its failure from
                     * inside an active user turn. Do not admit parent panic
                     * code until physical finalization and all owned turns
                     * are quiescent, even when other carriers are idle. */
                    OresNativeActorRef child = candidate->failure.child;
                    if (child.actor_slot >= s->actor_count) continue;
                    ActorLane* child_lane = &s->actors[child.actor_slot];
                    if (!child_lane->retired || child_lane->active
                        || child_lane->queued || child_lane->has_control)
                        continue;
                }
            } else if (!candidate->queued) {
                continue;
            }
            *actor = i;
            *control = pass;
            s->next_actor = (i + 1) % s->actor_count;
            return 1;
        }
    }
    return 0;
}
static void* carrier(void* raw) {
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)raw;
    for (;;) {
        lock(s);
        size_t i = 0;
        int control = 0;
        while (!find_ready(s, &i, &control)) {
            if (s->retired && !s->outstanding) {
                unlock(s);
                return NULL;
            }
            if (pthread_cond_wait(&s->ready, &s->lock) != 0) abort();
        }
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
        lane->active = 1;
        /* Copy the identity while holding the scheduler lock. The lane
         * cannot reincarnate while active, so delayed stale references
         * carry the ORIGINAL generation, never a newly reused slot. */
        const OresNativeActorRef identity = {
            s->domain, (uint64_t)i, lane->generation
        };
        unlock(s);

        executing = s;
        if (s->identity_callbacks) {
            if (typed_failure) {
                if (s->identity_panic_callback)
                    s->identity_panic_callback(s->context, identity, failure);
            } else if (control) {
                if (s->identity_control_callback)
                    s->identity_control_callback(s->context, identity, payload);
            } else {
                s->identity_callback(s->context, identity, payload);
            }
        } else if (typed_failure) {
            s->panic_callback(s->context, i, failure);
        } else if (control) {
            if (s->control_callback) s->control_callback(s->context, i, payload);
        } else {
            s->callback(s->context, i, payload);
        }
        executing = NULL;

        lock(s);
        if (!lane->active || !s->outstanding || s->completed == UINT64_MAX) abort();
        lane->active = 0;
        s->outstanding--;
        s->completed++;
        if (control) ++s->control_completed;
        /* Another lane's typed CONTROL may have waited on this actor's
         * final turn. Wake workers even if this lane is now empty. */
        pthread_cond_signal(&s->ready);
        if (lane->retired && !lane->active && !lane->queued && !lane->has_control)
            pthread_cond_broadcast(&s->drained);
        if (!s->outstanding) {
            pthread_cond_broadcast(&s->drained);
            if (s->retired) pthread_cond_broadcast(&s->ready);
        }
        unlock(s);
    }
}
static OresNativeActorScheduler* open_scheduler(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    OresNativeActorPanicFn panic_callback,
    OresNativeActorIdentityTaskFn identity_callback,
    OresNativeActorIdentityTaskFn identity_control_callback,
    OresNativeActorIdentityPanicFn identity_panic_callback,
    void* ctx) {
    if ((!callback && !identity_callback)
        || (callback && identity_callback)
        || !actors || actors > 1024 || !capacity || capacity > 65536
        || !workers || workers > 128 || actors > SIZE_MAX / capacity)
        return NULL;
    OresNativeActorScheduler* s = (OresNativeActorScheduler*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->actors = (ActorLane*)calloc(actors, sizeof(ActorLane));
    s->slots = (uint64_t*)calloc(actors * capacity, sizeof(uint64_t));
    s->threads = (pthread_t*)calloc(workers, sizeof(pthread_t));
    if (!s->actors || !s->slots || !s->threads) goto allocation_failed;
    s->domain = atomic_fetch_add_explicit(&next_scheduler_domain, 1, memory_order_relaxed);
    if (!s->domain || s->domain == UINT64_MAX) goto allocation_failed;
    s->actor_count = actors;
    s->per_actor_capacity = capacity;
    s->worker_count = workers;
    s->callback = callback;
    s->control_callback = control_callback;
    s->panic_callback = panic_callback;
    s->identity_callback = identity_callback;
    s->identity_control_callback = identity_control_callback;
    s->identity_panic_callback = identity_panic_callback;
    s->identity_callbacks = identity_callback != NULL;
    s->context = ctx;
    for (size_t i = 0; i < actors; ++i)
        s->actors[i].messages = s->slots + i * capacity;
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
    if (pthread_mutex_init(&s->restart_publication_gate, NULL) != 0) {
        pthread_cond_destroy(&s->drained);
        pthread_cond_destroy(&s->ready);
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    size_t started = 0;
    while (started < workers && pthread_create(&s->threads[started], NULL, carrier, s) == 0)
        ++started;
    if (started != workers) {
        ores_native_actor_scheduler_retire(s);
        for (size_t i = 0; i < started; ++i) pthread_join(s->threads[i], NULL);
        pthread_mutex_destroy(&s->restart_publication_gate);
        pthread_cond_destroy(&s->drained);
        pthread_cond_destroy(&s->ready);
        pthread_mutex_destroy(&s->lock);
        goto allocation_failed;
    }
    return s;
allocation_failed:
    free(s->threads);
    free(s->slots);
    free(s->actors);
    free(s);
    return NULL;
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_typed_control(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, OresNativeActorTaskFn control_callback,
    OresNativeActorPanicFn panic_callback, void* ctx) {
    return open_scheduler(actors, capacity, workers, callback,
        control_callback, panic_callback, NULL, NULL, NULL, ctx);
}
OresNativeActorScheduler* ores_native_actor_scheduler_open_identity_control(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorIdentityTaskFn callback,
    OresNativeActorIdentityTaskFn control_callback,
    OresNativeActorIdentityPanicFn panic_callback, void* ctx) {
    return open_scheduler(actors, capacity, workers, NULL, NULL, NULL,
        callback, control_callback, panic_callback, ctx);
}
int ores_native_actor_scheduler_uses_identity_callbacks(
    OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    const int identity = s->identity_callbacks != 0;
    unlock(s);
    return identity;
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
static int enqueue_locked(OresNativeActorScheduler* s, size_t i, uint64_t payload) {
    if (s->retired || i >= s->actor_count
        || !s->actors[i].spawned || !s->actors[i].published
        || s->actors[i].retired
        || s->actors[i].queued + s->actors[i].active >= s->per_actor_capacity
        || s->accepted == UINT64_MAX) {
        if (s->rejected != UINT64_MAX) s->rejected++;
        return 0;
    }
    ActorLane* a = &s->actors[i];
    a->messages[a->tail] = payload;
    a->tail = (a->tail + 1) % s->per_actor_capacity;
    a->queued++;
    s->accepted++;
    s->outstanding++;
    pthread_cond_signal(&s->ready);
    return 1;
}
/*
 * A stopped unpublished candidate is the only incarnation whose physical
 * slot can currently be recycled: it never entered a callback and cannot
 * have delivered a terminal PANIC. Reuse is atomic with STOP and admissions
 * under the publication gate; old ActorRefs fail matching() afterwards.
 * The full published-actor recycling protocol requires generation-bearing
 * native callback ABI and separate supervisor retirement proofs.
 */
OresNativeActorRef ores_native_actor_scheduler_recycle_unpublished_done(
    OresNativeActorScheduler* s, OresNativeActorRef old) {
    OresNativeActorRef missing = {0, 0, 0};
    if (!s || owned_restart_lease == s) return missing;
    if (pthread_mutex_lock(&s->restart_publication_gate) != 0) abort();
    lock(s);
    int allowed = !s->retired && matching(s, old);
    if (allowed) {
        ActorLane* a = &s->actors[old.actor_slot];
        allowed = a->retired && !a->published
            && !a->active && !a->queued && !a->has_control
            && !a->failure_attempted && !a->failure_published
            && a->restart_lease_nonce == 0 && a->generation < UINT64_MAX;
        if (allowed) {
            /* Empty all transport and classification state before bumping
             * generation. No physical mailbox datum survives an incarnation. */
            a->head = 0;
            a->tail = 0;
            a->control_payload = 0;
            a->control_is_failure = 0;
            a->failure = (OresNativeActorTerminalFailure){0};
            a->failure_attempted = 0;
            a->failure_published = 0;
            a->normal_exit_sealed = 0;
            a->terminal_parent = (OresNativeActorRef){0};
            a->terminal_failure = (OresNativeActorTerminalFailure){0};
            a->terminal_claim_nonce = 0;
            a->terminal_receipt_state = ORES_NATIVE_TERMINAL_RECEIPT_NONE;
            a->terminal_was_published = 0;
            a->restart_lease_nonce = 0;
            a->restart_lease_owner = (pthread_t){0};
            for (size_t j = 0; j < s->per_actor_capacity; ++j)
                a->messages[j] = 0;
            a->generation++;
            a->retired = 0;
            a->spawned = 1;
            a->published = 0;
            missing = (OresNativeActorRef){
                s->domain, old.actor_slot, a->generation
            };
        }
    }
    unlock(s);
    if (pthread_mutex_unlock(&s->restart_publication_gate) != 0) abort();
    return missing;
}
/*
 * Normal active-generation reuse is only permitted when no legacy callback
 * can have used a slot index. The host must first claim physical DONE and
 * seal that incarnation as a normal exit. A terminal PANIC attempt forever
 * excludes recycling here: receipt reconciliation belongs to supervisor.
 */
OresNativeActorRef ores_native_actor_scheduler_recycle_normal_done(
    OresNativeActorScheduler* s, OresNativeActorRef old) {
    OresNativeActorRef missing = {0, 0, 0};
    if (!s || owned_restart_lease == s) return missing;
    if (pthread_mutex_lock(&s->restart_publication_gate) != 0) abort();
    lock(s);
    int allowed = !s->retired && s->identity_callbacks && matching(s, old);
    if (allowed) {
        ActorLane* a = &s->actors[old.actor_slot];
        allowed = a->retired && a->published && a->normal_exit_sealed
            && !a->active && !a->queued && !a->has_control
            && !a->failure_attempted && !a->failure_published
            && !a->restart_lease_nonce && a->generation < UINT64_MAX;
        if (allowed) {
            a->head = a->tail = a->queued = 0;
            a->control_payload = 0;
            a->control_is_failure = 0;
            a->failure = (OresNativeActorTerminalFailure){0};
            a->failure_attempted = a->failure_published = 0;
            a->normal_exit_sealed = 0;
            a->terminal_parent = (OresNativeActorRef){0};
            a->terminal_failure = (OresNativeActorTerminalFailure){0};
            a->terminal_claim_nonce = 0;
            a->terminal_receipt_state = ORES_NATIVE_TERMINAL_RECEIPT_NONE;
            a->terminal_was_published = 0;
            a->restart_lease_owner = (pthread_t){0};
            for (size_t j = 0; j < s->per_actor_capacity; ++j)
                a->messages[j] = 0;
            a->generation++;
            a->spawned = 1;
            a->retired = 0;
            a->published = 0;
            missing = (OresNativeActorRef){
                s->domain, old.actor_slot, a->generation
            };
        }
    }
    unlock(s);
    if (pthread_mutex_unlock(&s->restart_publication_gate) != 0) abort();
    return missing;
}
/* A replacement must use the unpublished variant so neither ref-based
 * send nor legacy index submit can bypass the bridge's identity commit. */
static OresNativeActorRef spawn_internal(
    OresNativeActorScheduler* s, size_t i, unsigned published) {
    OresNativeActorRef missing = {0, 0, 0};
    if (!s) return missing;
    lock(s);
    if (s->retired || i >= s->actor_count || s->actors[i].spawned) {
        unlock(s);
        return missing;
    }
    ActorLane* a = &s->actors[i];
    a->spawned = 1;
    a->published = published;
    a->generation = 1;
    OresNativeActorRef ref = {s->domain, (uint64_t)i, a->generation};
    unlock(s);
    return ref;
}
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* s, size_t i) {
    return spawn_internal(s, i, 1);
}
OresNativeActorRef ores_native_actor_scheduler_spawn_unpublished(
    OresNativeActorScheduler* s, size_t i) {
    return spawn_internal(s, i, 0);
}
/* Registration and restart use the same scheduler publication gate.
 * Only expected_published differentiates initial host registration from
 * replacement publication, neither permits STOP until the lease releases. */
static int acquire_admission_lease(
    OresNativeActorScheduler* s, OresNativeActorRef actor,
    OresNativeRestartLease* out, int expected_published) {
    if (!s || !out || owned_restart_lease) return 0;
    /* Same gate as physical STOP and whole-scheduler retirement. */
    if (pthread_mutex_lock(&s->restart_publication_gate) != 0) abort();
    lock(s);
    int ok = matching(s, actor) && !s->retired
        && !s->actors[actor.actor_slot].retired
        && s->actors[actor.actor_slot].published == (unsigned)expected_published
        && !s->actors[actor.actor_slot].restart_lease_nonce
        && s->next_restart_lease_nonce != UINT64_MAX;
    if (ok) {
        ActorLane* a = &s->actors[actor.actor_slot];
        a->restart_lease_nonce = ++s->next_restart_lease_nonce;
        a->restart_lease_owner = pthread_self();
        owned_restart_lease = s;
        out->actor = actor;
        out->nonce = a->restart_lease_nonce;
    }
    unlock(s);
    if (!ok && pthread_mutex_unlock(&s->restart_publication_gate) != 0) abort();
    return ok;
}
int ores_native_actor_scheduler_acquire_restart_lease(
    OresNativeActorScheduler* s, OresNativeActorRef actor,
    OresNativeRestartLease* out) {
    return acquire_admission_lease(s, actor, out, 0);
}
int ores_native_actor_scheduler_acquire_registration_lease(
    OresNativeActorScheduler* s, OresNativeActorRef actor,
    OresNativeRestartLease* out) {
    return acquire_admission_lease(s, actor, out, 1);
}
static int matches_restart_lease(OresNativeActorScheduler* s,
                                  OresNativeRestartLease lease) {
    return lease.nonce && matching(s, lease.actor)
        && s->actors[lease.actor.actor_slot].restart_lease_nonce == lease.nonce
        && pthread_equal(s->actors[lease.actor.actor_slot].restart_lease_owner,
                         pthread_self());
}
int ores_native_actor_scheduler_publish_restart_lease(
    OresNativeActorScheduler* s, OresNativeRestartLease lease) {
    if (!s || owned_restart_lease != s) return 0;
    lock(s);
    int ok = matches_restart_lease(s, lease) && !s->retired
        && !s->actors[lease.actor.actor_slot].retired
        && !s->actors[lease.actor.actor_slot].published;
    if (ok) s->actors[lease.actor.actor_slot].published = 1;
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_release_restart_lease(
    OresNativeActorScheduler* s, OresNativeRestartLease lease) {
    if (!s || owned_restart_lease != s) return 0;
    lock(s);
    int ok = matches_restart_lease(s, lease);
    if (ok) s->actors[lease.actor.actor_slot].restart_lease_nonce = 0;
    unlock(s);
    if (ok) {
        owned_restart_lease = NULL;
        if (pthread_mutex_unlock(&s->restart_publication_gate) != 0) abort();
    }
    return ok;
}
int ores_native_actor_scheduler_send(OresNativeActorScheduler* s,
                                     OresNativeActorRef actor, uint64_t payload) {
    if (!s) return 0;
    lock(s);
    int ok = matching(s, actor)
        ? enqueue_locked(s, (size_t)actor.actor_slot, payload) : 0;
    if (!matching(s, actor) && s->rejected != UINT64_MAX) s->rejected++;
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_submit(OresNativeActorScheduler* s,
                                       size_t i, uint64_t message) {
    if (!s) return 0;
    lock(s);
    /* This legacy API carries only a slot index. It cannot distinguish
     * an obsolete caller after a slot's first physical reincarnation.
     * Fail closed; generation-aware callers MUST use send(ActorRef). */
    int ok;
    if (i < s->actor_count && s->actors[i].generation > 1) {
        if (s->rejected != UINT64_MAX) s->rejected++;
        ok = 0;
    } else {
        ok = enqueue_locked(s, i, message);
    }
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_publish_control(
    OresNativeActorScheduler* s, OresNativeActorRef parent, uint64_t payload) {
    if (!s) return 0;
    lock(s);
    int ok = !s->retired
        && (s->identity_callbacks ? s->identity_control_callback != NULL
                                  : s->control_callback != NULL)
        && matching(s, parent)
        && s->actors[parent.actor_slot].published
        && !s->actors[parent.actor_slot].retired
        && !s->actors[parent.actor_slot].has_control
        && s->accepted != UINT64_MAX;
    if (ok) {
        ActorLane* lane = &s->actors[parent.actor_slot];
        lane->control_payload = payload;
        lane->control_is_failure = 0;
        lane->has_control = 1;
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
    /* A valid terminal failure claim remains terminal even if the parent
     * CONTROL queue is saturated or the parent has already retired. The
     * caller must escalate/retry; it must never relabel the child as normal.
     */
    int attempted =
        (s->identity_callbacks ? s->identity_panic_callback != NULL
                               : s->panic_callback != NULL)
        && matching(s, parent) && matching(s, failure.child)
        && s->actors[failure.child.actor_slot].published
        && s->actors[failure.child.actor_slot].retired
        && !s->actors[failure.child.actor_slot].normal_exit_sealed
        && parent.actor_slot != failure.child.actor_slot
        && failure.kind >= ORES_NATIVE_CONTROL_PANIC
        && failure.kind <= ORES_NATIVE_CONTROL_STOPPED;
    /* Publication failure must not erase the first exact terminal
     * envelope. Record BEFORE deciding whether parent CONTROL is open.
     * Repeated attempts do not publish again: reconciliation owns retries. */
    if (attempted) {
        ActorLane* child = &s->actors[failure.child.actor_slot];
        if (child->failure_attempted) {
            attempted = 0;
        } else {
            child->failure_attempted = 1;
            child->terminal_parent = parent;
            child->terminal_failure = failure;
            child->terminal_receipt_state = ORES_NATIVE_TERMINAL_RECEIPT_RECORDED;
        }
    }
    int ok = attempted && !s->retired
        && s->actors[parent.actor_slot].published
        && !s->actors[failure.child.actor_slot].failure_published
        && !s->actors[parent.actor_slot].retired
        && !s->actors[parent.actor_slot].has_control
        && s->accepted != UINT64_MAX;
    if (ok) {
        /* Claim terminal failure atomically against normal-exit sealing. */
        s->actors[failure.child.actor_slot].failure_published = 1;
        s->actors[failure.child.actor_slot].terminal_was_published = 1;
        ActorLane* a = &s->actors[parent.actor_slot];
        a->failure = failure;
        a->control_is_failure = 1;
        a->has_control = 1;
        s->accepted++;
        s->outstanding++;
        pthread_cond_signal(&s->ready);
    } else if (s->rejected != UINT64_MAX) s->rejected++;
    unlock(s);
    return ok;
}
/* Claims and acknowledgments are linearized by the same scheduler lock
 * as terminal publication and physical DONE. They never dispatch host
 * policy and never retry a possibly committed external side effect. */
static int same_terminal_actor(OresNativeActorRef a, OresNativeActorRef b) {
    return a.scheduler_domain == b.scheduler_domain
        && a.actor_slot == b.actor_slot && a.generation == b.generation;
}
static int terminal_receipt_locked(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorRef child, OresNativeTerminalReceipt* out) {
    if (!matching(s, child)) return 0;
    ActorLane* lane = &s->actors[child.actor_slot];
    if (lane->terminal_receipt_state == ORES_NATIVE_TERMINAL_RECEIPT_NONE
        || !same_terminal_actor(lane->terminal_parent, parent)
        || !same_terminal_actor(lane->terminal_failure.child, child))
        return 0;
    if (out) {
        *out = (OresNativeTerminalReceipt){
            lane->terminal_parent, lane->terminal_failure,
            lane->terminal_claim_nonce, lane->terminal_receipt_state,
            lane->terminal_was_published
        };
    }
    return 1;
}
int ores_native_actor_scheduler_terminal_receipt(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorRef child, OresNativeTerminalReceipt* out) {
    if (!s || !out) return 0;
    lock(s);
    int ok = terminal_receipt_locked(s, parent, child, out);
    unlock(s);
    return ok;
}
/* Shared generation-bound, one-use claim. The ordinary parent CONTROL
 * path needs a live published parent and a scheduler-published terminal
 * envelope. Recovery is deliberately separate: its original parent may
 * already be STOPPED and its initial publication may have failed. */
static int claim_terminal_receipt_internal(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorRef child, OresNativeTerminalReceipt* out,
    int require_live_parent) {
    if (!s || !out) return 0;
    lock(s);
    int ok = terminal_receipt_locked(s, parent, child, NULL);
    if (ok && require_live_parent) {
        ok = !s->retired && matching(s, parent)
            && s->actors[parent.actor_slot].published
            && !s->actors[parent.actor_slot].retired
            && s->actors[child.actor_slot].terminal_was_published;
    }
    if (ok) {
        ActorLane* lane = &s->actors[child.actor_slot];
        ok = lane->terminal_receipt_state == ORES_NATIVE_TERMINAL_RECEIPT_RECORDED
            && lane->retired && !lane->active && !lane->queued
            && !lane->has_control && s->next_terminal_claim_nonce != UINT64_MAX;
        if (ok) {
            /* The exact claim and parent STOP share the scheduler mutex.
             * A later STOP cannot invalidate an already-claimed in-flight
             * host effect, but a STOP that wins first denies ordinary
             * callback admission without consuming its RECORDED evidence. */
            uint64_t nonce = ++s->next_terminal_claim_nonce;
            lane->terminal_claim_nonce = nonce;
            lane->terminal_receipt_state = ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED;
            ok = terminal_receipt_locked(s, parent, child, out);
        }
    }
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_claim_terminal_receipt(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorRef child, OresNativeTerminalReceipt* out) {
    /* Trusted recovered-parent claim, including a parent that stopped
     * before its CONTROL callback could be accepted. */
    return claim_terminal_receipt_internal(s, parent, child, out, 0);
}
int ores_native_actor_scheduler_claim_live_parent_terminal_receipt(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorRef child, OresNativeTerminalReceipt* out) {
    return claim_terminal_receipt_internal(s, parent, child, out, 1);
}
int ores_native_actor_scheduler_ack_terminal_receipt(
    OresNativeActorScheduler* s, OresNativeTerminalReceipt receipt) {
    if (!s || receipt.status != ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
        || !receipt.nonce) return 0;
    lock(s);
    int ok = terminal_receipt_locked(s, receipt.original_parent,
                                     receipt.failure.child, NULL);
    if (ok) {
        ActorLane* lane = &s->actors[receipt.failure.child.actor_slot];
        const OresNativeActorTerminalFailure f = lane->terminal_failure;
        ok = lane->terminal_receipt_state == ORES_NATIVE_TERMINAL_RECEIPT_CLAIMED
            && lane->terminal_claim_nonce == receipt.nonce
            && f.kind == receipt.failure.kind
            && f.failure_code == receipt.failure.failure_code
            && f.correlation == receipt.failure.correlation
            && same_terminal_actor(f.child, receipt.failure.child);
        if (ok)
            lane->terminal_receipt_state = ORES_NATIVE_TERMINAL_RECEIPT_ACKNOWLEDGED;
    }
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
    if (!s || owned_restart_lease == s) return 0;
    if (pthread_mutex_lock(&s->restart_publication_gate) != 0) abort();
    lock(s);
    int ok = matching(s, actor) && !s->actors[actor.actor_slot].retired;
    if (ok) s->actors[actor.actor_slot].retired = 1;
    unlock(s);
    if (pthread_mutex_unlock(&s->restart_publication_gate) != 0) abort();
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
int ores_native_actor_scheduler_poll_published(OresNativeActorScheduler* s,
                                               OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int ready = matching(s, actor) && !s->retired
        && !s->actors[actor.actor_slot].retired
        && s->actors[actor.actor_slot].published;
    unlock(s);
    return ready;
}
int ores_native_actor_scheduler_poll_done(OresNativeActorScheduler* s,
                                          OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int done = matching(s, actor) && s->actors[actor.actor_slot].retired
        && !s->actors[actor.actor_slot].queued && !s->actors[actor.actor_slot].has_control
        && !s->actors[actor.actor_slot].active;
    unlock(s);
    return done;
}
int ores_native_actor_scheduler_seal_normal_exit(
    OresNativeActorScheduler* s, OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int ok = matching(s, actor);
    if (ok) {
        ActorLane* a = &s->actors[actor.actor_slot];
        ok = a->retired && !a->active && !a->queued && !a->has_control
            && !a->failure_attempted && !a->normal_exit_sealed;
        if (ok) a->normal_exit_sealed = 1;
    }
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_wait_done(OresNativeActorScheduler* s,
                                          OresNativeActorRef actor) {
    /* A same-thread admission lease pins the publication gate. A pending
     * callback may need STOP/retirement before physical DONE; waiting under
     * that lease can create a cross-thread cycle. Fail closed, even if the
     * requested actor is already DONE, to keep this host API nonblocking. */
    if (!s || executing == s || owned_restart_lease == s) return 0;
    lock(s);
    if (!matching(s, actor) || !s->actors[actor.actor_slot].retired) {
        unlock(s);
        return 0;
    }
    ActorLane* lane = &s->actors[actor.actor_slot];
    while (lane->queued || lane->has_control || lane->active) {
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    }
    unlock(s);
    return 1;
}
void ores_native_actor_scheduler_retire(OresNativeActorScheduler* s) {
    /* This API has no error return. Fail closed instead of deadlocking a
     * trusted host thread that already holds the scheduler publication gate. */
    if (!s || owned_restart_lease == s) abort();
    if (pthread_mutex_lock(&s->restart_publication_gate) != 0) abort();
    lock(s);
    s->retired = 1;
    for (size_t i = 0; i < s->actor_count; ++i) s->actors[i].retired = 1;
    pthread_cond_broadcast(&s->ready);
    unlock(s);
    if (pthread_mutex_unlock(&s->restart_publication_gate) != 0) abort();
}
int ores_native_actor_scheduler_drain(OresNativeActorScheduler* s) {
    /* Draining from the lease owner can depend on a STOP blocked by this
     * thread's own publication gate. It must never enter cond_wait. */
    if (!s || executing == s || owned_restart_lease == s) return 0;
    lock(s);
    while (s->outstanding)
        if (pthread_cond_wait(&s->drained, &s->lock) != 0) abort();
    unlock(s);
    return 1;
}
int ores_native_actor_scheduler_try_destroy(OresNativeActorScheduler* s) {
    if (!s || executing == s || owned_restart_lease == s) return 0;
    if (pthread_mutex_lock(&s->restart_publication_gate) != 0) abort();
    lock(s);
    int ready = s->retired && !s->outstanding;
    if (ready) pthread_cond_broadcast(&s->ready);
    unlock(s);
    if (!ready) {
        if (pthread_mutex_unlock(&s->restart_publication_gate) != 0) abort();
        return 0;
    }
    /* Caller must externally fence producers; no concurrent API calls now. */
    for (size_t i = 0; i < s->worker_count; ++i)
        if (pthread_join(s->threads[i], NULL) != 0) abort();
    if (pthread_cond_destroy(&s->drained) != 0
        || pthread_cond_destroy(&s->ready) != 0
        || pthread_mutex_destroy(&s->lock) != 0) abort();
    if (pthread_mutex_unlock(&s->restart_publication_gate) != 0
        || pthread_mutex_destroy(&s->restart_publication_gate) != 0) abort();
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
uint64_t ores_native_actor_scheduler_accepted(OresNativeActorScheduler* s) {
    return counter(s, 0);
}
uint64_t ores_native_actor_scheduler_rejected(OresNativeActorScheduler* s) {
    return counter(s, 1);
}
uint64_t ores_native_actor_scheduler_completed(OresNativeActorScheduler* s) {
    return counter(s, 2);
}
