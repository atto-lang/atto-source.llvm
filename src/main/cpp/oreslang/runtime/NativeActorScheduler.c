#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <stdatomic.h>

typedef struct {
    int subscribed, emitted;
    OresNativeActorRef parent;
    uint64_t ticket;
} FutureObserver;
typedef struct {
    uint64_t* messages; /* fixed ring, assigned at scheduler startup */
    size_t head, tail, queued;
    unsigned active;
    unsigned spawned, retired;
    uint64_t generation;
    unsigned init_complete, final_complete;
    FutureObserver ready_future, done_future;
    unsigned control_pending, control_inflight;
    OresNativeActorControlKind control_kind;
    OresNativeActorRef control_child;
    uint64_t control_payload;
} ActorLane;
struct OresNativeActorScheduler {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_cond_t drained;
    ActorLane* actors;
    uint64_t* slots;
    pthread_t* threads;
    size_t actor_count, per_actor_capacity, worker_count;
    size_t next_actor, outstanding;
    uint64_t domain;
    uint64_t accepted, rejected, completed;
    uint64_t control_accepted, control_completed;
    OresNativeActorControlFn control_callback;
    int retired;
    OresNativeActorTaskFn callback;
    void* context;
};
static _Thread_local OresNativeActorScheduler* executing;
static _Thread_local size_t executing_actor = SIZE_MAX;
static _Atomic(uint64_t) next_scheduler_domain = 1;

static void lock(OresNativeActorScheduler* s) {
    if (pthread_mutex_lock(&s->lock) != 0) abort();
}
static void unlock(OresNativeActorScheduler* s) {
    if (pthread_mutex_unlock(&s->lock) != 0) abort();
}
static int matching(OresNativeActorScheduler* s, OresNativeActorRef ref);
static void pump_futures_for_parent(OresNativeActorScheduler* s, size_t parent);
static int post_control_locked(OresNativeActorScheduler* s, OresNativeActorRef parent,
                               OresNativeActorRef child, OresNativeActorControlKind kind,
                               uint64_t payload);
static int find_ready(OresNativeActorScheduler* s, size_t* actor) {
    for (size_t offset = 0; offset < s->actor_count; ++offset) {
        size_t i = (s->next_actor + offset) % s->actor_count;
        if ((s->actors[i].control_pending || s->actors[i].queued) && !s->actors[i].active) {
            *actor = i;
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
        while (!find_ready(s, &i)) {
            if (s->retired && !s->outstanding) {
                unlock(s);
                return NULL;
            }
            if (pthread_cond_wait(&s->ready, &s->lock) != 0) abort();
        }
        ActorLane* lane = &s->actors[i];
        const int is_control = lane->control_pending != 0;
        uint64_t payload = 0;
        OresNativeActorControlKind kind = ORES_ACTOR_CONTROL_READY;
        OresNativeActorRef child = {0, 0, 0};
        if (is_control) {
            if (!s->control_callback || lane->control_inflight) abort();
            kind = lane->control_kind;
            child = lane->control_child;
            payload = lane->control_payload;
            lane->control_pending = 0;
            lane->control_inflight = 1;
        } else {
            payload = lane->messages[lane->head];
            lane->head = (lane->head + 1) % s->per_actor_capacity;
            lane->queued--;
        }
        lane->active = 1;
        unlock(s);

        executing = s;
        executing_actor = i;
        if (is_control) s->control_callback(s->context, i, kind, child, payload);
        else s->callback(s->context, i, payload);
        executing_actor = SIZE_MAX;
        executing = NULL;

        lock(s);
        if (!lane->active || !s->outstanding || s->completed == UINT64_MAX) abort();
        lane->active = 0;
        s->outstanding--;
        s->completed++;
        if (is_control) {
            if (!lane->control_inflight || s->control_completed == UINT64_MAX) abort();
            lane->control_inflight = 0;
            s->control_completed++;
            /* Deferred future notifications own their state until admitted. */
            pump_futures_for_parent(s, i);
        }
        if (lane->control_pending || lane->queued) pthread_cond_signal(&s->ready);
        if (!s->outstanding) {
            pthread_cond_broadcast(&s->drained);
            if (s->retired) pthread_cond_broadcast(&s->ready);
        }
        unlock(s);
    }
}
OresNativeActorScheduler* ores_native_actor_scheduler_open(
    size_t actors, size_t capacity, size_t workers,
    OresNativeActorTaskFn callback, void* ctx) {
    if (!callback || !actors || actors > 1024 || !capacity || capacity > 65536
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
    size_t started = 0;
    while (started < workers && pthread_create(&s->threads[started], NULL, carrier, s) == 0)
        ++started;
    if (started != workers) {
        ores_native_actor_scheduler_retire(s);
        for (size_t i = 0; i < started; ++i) pthread_join(s->threads[i], NULL);
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
static int matching(OresNativeActorScheduler* s, OresNativeActorRef ref) {
    return s && ref.scheduler_domain == s->domain
        && ref.actor_slot < s->actor_count
        && ref.generation != 0
        && s->actors[ref.actor_slot].spawned
        && s->actors[ref.actor_slot].generation == ref.generation;
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
    a->messages[a->tail] = payload;
    a->tail = (a->tail + 1) % s->per_actor_capacity;
    a->queued++;
    s->accepted++;
    s->outstanding++;
    pthread_cond_signal(&s->ready);
    return 1;
}
static OresNativeActorRef spawn_slot(OresNativeActorScheduler* s, size_t i,
                                     int initialized) {
    OresNativeActorRef missing = {0, 0, 0};
    if (!s) return missing;
    lock(s);
    if (s->retired || i >= s->actor_count || s->actors[i].spawned) {
        unlock(s);
        return missing;
    }
    ActorLane* a = &s->actors[i];
    a->spawned = 1;
    a->generation = 1;
    a->init_complete = initialized != 0;
    a->final_complete = 0;
    OresNativeActorRef ref = {s->domain, (uint64_t)i, a->generation};
    unlock(s);
    return ref;
}
OresNativeActorRef ores_native_actor_scheduler_spawn(
    OresNativeActorScheduler* s, size_t i) {
    /* Legacy synchronous host start, not compiler-lowered async initialization. */
    return spawn_slot(s, i, 1);
}
OresNativeActorRef ores_native_actor_scheduler_spawn_pending(
    OresNativeActorScheduler* s, size_t i) {
    return spawn_slot(s, i, 0);
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
    int ok = enqueue_locked(s, i, message);
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_stop(OresNativeActorScheduler* s,
                                     OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    if (!matching(s, actor) || s->actors[actor.actor_slot].retired) {
        unlock(s); return 0;
    }
    s->actors[actor.actor_slot].retired = 1;
    unlock(s);
    return 1;
}
int ores_native_actor_scheduler_poll_ready(OresNativeActorScheduler* s,
                                           OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int ready = matching(s, actor) && !s->retired\n        && s->actors[actor.actor_slot].init_complete\n        && !s->actors[actor.actor_slot].retired;
    unlock(s);
    return ready;
}
int ores_native_actor_scheduler_poll_done(OresNativeActorScheduler* s,
                                          OresNativeActorRef actor) {
    if (!s) return 0;
    lock(s);
    int done = matching(s, actor) && s->actors[actor.actor_slot].retired
        && !s->actors[actor.actor_slot].queued && !s->actors[actor.actor_slot].active;
    unlock(s);
    return done;
}

int ores_native_actor_scheduler_set_control_handler(
    OresNativeActorScheduler* s, OresNativeActorControlFn callback) {
    if (!s || !callback || executing == s) return 0;
    lock(s);
    int ok = !s->retired && !s->control_callback && !s->outstanding;
    if (ok) s->control_callback = callback;
    unlock(s);
    return ok;
}
static int post_control_locked(OresNativeActorScheduler* s,
    OresNativeActorRef parent, OresNativeActorRef child,
    OresNativeActorControlKind kind, uint64_t payload) {
    int valid = s->control_callback && !s->retired
        && matching(s, parent) && matching(s, child)
        && parent.actor_slot != child.actor_slot
        && !s->actors[parent.actor_slot].retired
        && kind >= ORES_ACTOR_CONTROL_READY && kind <= ORES_ACTOR_CONTROL_CANCELLED;
    if (!valid) return 0;
    ActorLane* lane = &s->actors[parent.actor_slot];
    if (lane->control_pending || lane->control_inflight
        || s->control_accepted == UINT64_MAX || s->accepted == UINT64_MAX)
        return 0;
    lane->control_pending = 1;
    lane->control_kind = kind;
    lane->control_child = child;
    lane->control_payload = payload;
    s->control_accepted++;
    s->accepted++;
    s->outstanding++;
    pthread_cond_signal(&s->ready);
    return 1;
}
int ores_native_actor_scheduler_post_control(
    OresNativeActorScheduler* s, OresNativeActorRef parent,
    OresNativeActorRef child, OresNativeActorControlKind kind, uint64_t payload) {
    if (!s) return 0;
    lock(s);
    int ok = post_control_locked(s, parent, child, kind, payload);
    unlock(s);
    return ok;
}
uint64_t ores_native_actor_scheduler_control_accepted(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    uint64_t result = s->control_accepted;
    unlock(s);
    return result;
}
uint64_t ores_native_actor_scheduler_control_completed(OresNativeActorScheduler* s) {
    if (!s) return 0;
    lock(s);
    uint64_t result = s->control_completed;
    unlock(s);
    return result;
}

static int future_state_locked(OresNativeActorScheduler* s, OresNativeActorFuture future) {
    if (!matching(s, future.child)) return -1;
    const ActorLane* a = &s->actors[future.child.actor_slot];
    if (future.stage == ORES_NATIVE_FUTURE_READY) return a->init_complete ? 1 : 0;
    if (future.stage == ORES_NATIVE_FUTURE_DONE) return a->final_complete ? 1 : 0;
    return -1;
}
static void try_publish_future(OresNativeActorScheduler* s, size_t child_idx,
                               OresNativeFutureStage stage) {
    ActorLane* child = &s->actors[child_idx];
    FutureObserver* observer = stage == ORES_NATIVE_FUTURE_READY
        ? &child->ready_future : &child->done_future;
    const int finished = stage == ORES_NATIVE_FUTURE_READY
        ? child->init_complete : child->final_complete;
    if (!observer->subscribed || observer->emitted || !finished) return;
    const OresNativeActorRef source = {s->domain, (uint64_t)child_idx, child->generation};
    const OresNativeActorControlKind reason = stage == ORES_NATIVE_FUTURE_READY
        ? ORES_ACTOR_CONTROL_READY : ORES_ACTOR_CONTROL_DONE;
    if (post_control_locked(s, observer->parent, source, reason, observer->ticket))
        observer->emitted = 1;
}
static void pump_futures_for_parent(OresNativeActorScheduler* s, size_t parent) {
    /* Rare supervisor CONTROL completion path; O(actor_count) scan,
     * no allocation, no loss of registered futures when CONTROL is busy. */
    for (size_t i = 0; i < s->actor_count; ++i) {
        ActorLane* child = &s->actors[i];
        if (child->ready_future.subscribed
            && child->ready_future.parent.actor_slot == parent)
            try_publish_future(s, i, ORES_NATIVE_FUTURE_READY);
        if (child->done_future.subscribed
            && child->done_future.parent.actor_slot == parent)
            try_publish_future(s, i, ORES_NATIVE_FUTURE_DONE);
        if (s->actors[parent].control_pending) break;
    }
}
int ores_native_actor_scheduler_mark_ready(
    OresNativeActorScheduler* s, OresNativeActorRef ref) {
    if (!s || executing != s || executing_actor != ref.actor_slot) return 0;
    lock(s);
    int ok = matching(s, ref) && !s->actors[ref.actor_slot].init_complete
        && !s->actors[ref.actor_slot].retired;
    if (ok) {
        s->actors[ref.actor_slot].init_complete = 1;
        try_publish_future(s, (size_t)ref.actor_slot, ORES_NATIVE_FUTURE_READY);
    }
    unlock(s);
    return ok;
}
int ores_native_actor_scheduler_mark_finalized(
    OresNativeActorScheduler* s, OresNativeActorRef ref) {
    if (!s || executing == s) return 0;
    lock(s);
    int ok = matching(s, ref) && s->actors[ref.actor_slot].retired
        && !s->actors[ref.actor_slot].queued && !s->actors[ref.actor_slot].active
        && !s->actors[ref.actor_slot].final_complete;
    if (ok) {
        /* Caller must have released the actor's owned native resources. */
        s->actors[ref.actor_slot].final_complete = 1;
        try_publish_future(s, (size_t)ref.actor_slot, ORES_NATIVE_FUTURE_DONE);
    }
    unlock(s);
    return ok;
}
static OresNativeActorFuture make_future(OresNativeActorScheduler* s,
                                        OresNativeActorRef ref,
                                        OresNativeFutureStage stage) {
    OresNativeActorFuture invalid = {{0, 0, 0}, stage};
    if (!s) return invalid;
    lock(s);
    if (matching(s, ref)) invalid.child = ref;
    unlock(s);
    return invalid;
}
OresNativeActorFuture ores_native_actor_scheduler_get_ready(
    OresNativeActorScheduler* s, OresNativeActorRef ref) {
    return make_future(s, ref, ORES_NATIVE_FUTURE_READY);
}
OresNativeActorFuture ores_native_actor_scheduler_get_done(
    OresNativeActorScheduler* s, OresNativeActorRef ref) {
    return make_future(s, ref, ORES_NATIVE_FUTURE_DONE);
}
int ores_native_actor_future_is_complete(OresNativeActorScheduler* s,
                                         OresNativeActorFuture future) {
    if (!s) return -1;
    lock(s);
    int result = future_state_locked(s, future);
    unlock(s);
    return result;
}
int ores_native_actor_future_subscribe(
    OresNativeActorScheduler* s, OresNativeActorFuture future,
    OresNativeActorRef parent, uint64_t ticket) {
    if (!s) return 0;
    lock(s);
    int valid = !s->retired && s->control_callback && matching(s, parent)
        && !s->actors[parent.actor_slot].retired
        && future_state_locked(s, future) >= 0
        && parent.actor_slot != future.child.actor_slot;
    if (!valid) { unlock(s); return 0; }
    ActorLane* child = &s->actors[future.child.actor_slot];
    FutureObserver* watcher = future.stage == ORES_NATIVE_FUTURE_READY
        ? &child->ready_future : &child->done_future;
    if (watcher->subscribed) { unlock(s); return 0; }
    watcher->subscribed = 1;
    watcher->parent = parent;
    watcher->ticket = ticket;
    try_publish_future(s, (size_t)future.child.actor_slot, future.stage);
    unlock(s);
    return 1;
}

void ores_native_actor_scheduler_retire(OresNativeActorScheduler* s) {
    if (!s) abort();
    lock(s);
    s->retired = 1;
    for (size_t i = 0; i < s->actor_count; ++i) s->actors[i].retired = 1;
    pthread_cond_broadcast(&s->ready);
    unlock(s);
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
    if (ready) pthread_cond_broadcast(&s->ready);
    unlock(s);
    if (!ready) return 0;
    /* Caller must externally fence producers; no concurrent API calls now. */
    for (size_t i = 0; i < s->worker_count; ++i)
        if (pthread_join(s->threads[i], NULL) != 0) abort();
    if (pthread_cond_destroy(&s->drained) != 0
        || pthread_cond_destroy(&s->ready) != 0
        || pthread_mutex_destroy(&s->lock) != 0) abort();
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
