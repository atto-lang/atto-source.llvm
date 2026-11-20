#define _GNU_SOURCE
#include "atto/host_fs_async.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { FREE = 0, QUEUED = 1, RUNNING = 2, DONE = 3 };
typedef struct job {
    uint32_t generation;
    int state;
    int write;
    int cancelled;
    int cancelled_running_write;
    int abandoned;
    int outcome;
    size_t length;
    size_t used;
    uint64_t sequence;
    atto_fs_host_actor owner;
    char *path;
    uint8_t *bytes;
} job;
struct atto_fs_async_service {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t worker;
    int stopping;
    unsigned max_jobs;
    uint64_t next_sequence;
    atto_fs_broker *broker;
    uint64_t admitted_tenant;
    uint64_t admitted_generation;
    job slots[ATTO_FS_ASYNC_MAX_JOBS];
};
static int same_owner(const atto_fs_host_actor *a, const atto_fs_host_actor *b) {
    return a && b && a->tenant_id == b->tenant_id &&
           a->tenant_generation == b->tenant_generation &&
           a->actor_ref == b->actor_ref && a->actor_generation == b->actor_generation;
}
static int valid_actor(const atto_fs_async_service *s, const atto_fs_host_actor *a) {
    return s && a && s->broker &&
        s->broker->policy_layer_count == ATTO_FS_POLICY_LAYER_COUNT &&
        a->tenant_id && a->tenant_generation && a->actor_ref && a->actor_generation &&
        a->tenant_id == s->admitted_tenant &&
        a->tenant_generation == s->admitted_generation;
}
static void release_job(job *j) {
    free(j->path); free(j->bytes);
    j->path = NULL; j->bytes = NULL;
    j->state = FREE; j->cancelled = 0; j->cancelled_running_write = 0;
    j->abandoned = 0;
    j->length = 0; j->used = 0; j->outcome = 0;
    j->sequence = 0;
    memset(&j->owner, 0, sizeof(j->owner));
}
static job *find_ticket(atto_fs_async_service *s, uint64_t ticket) {
    const unsigned id = (unsigned)(ticket & 0xffffu);
    const uint64_t generation = ticket >> 16u;
    if (id == 0u || id > s->max_jobs || generation == 0u) return NULL;
    job *j = &s->slots[id - 1u];
    if (j->state == FREE || j->generation != generation) return NULL;
    return j;
}
static int enqueue(atto_fs_async_service *s, const atto_fs_host_actor *actor,
                   const char *path, const uint8_t *payload, size_t length,
                   int write, uint64_t *ticket) {
    if (ticket) *ticket = 0;
    if (!ticket || !valid_actor(s, actor) || !path ||
        (write && !payload && length) || length > ATTO_FS_MAX_BYTES) return -EACCES;
    const size_t path_len = strnlen(path, ATTO_FS_SCOPE_LIMIT);
    if (!path_len || path_len == ATTO_FS_SCOPE_LIMIT) return -EINVAL;
    char *owned_path = (char *)malloc(path_len + 1u);
    uint8_t *bytes = (uint8_t *)malloc(length + 1u);
    if (!owned_path || !bytes) { free(owned_path); free(bytes); return -ENOMEM; }
    memcpy(owned_path, path, path_len + 1u);
    if (write && length) memcpy(bytes, payload, length);
    int rc = -EAGAIN;
    pthread_mutex_lock(&s->lock);
    if (!s->stopping && valid_actor(s, actor)) {
        for (unsigned i = 0; i < s->max_jobs; ++i) {
            job *j = &s->slots[i];
            if (j->state != FREE || j->generation == UINT32_MAX) continue;
            ++j->generation;
            j->state = QUEUED;
            j->write = write; j->owner = *actor;
            j->path = owned_path; j->bytes = bytes;
            j->length = length; j->used = 0;
            j->cancelled = 0; j->cancelled_running_write = 0;
            j->abandoned = 0; j->outcome = 0;
            j->sequence = ++s->next_sequence;
            *ticket = ((uint64_t)j->generation << 16u) | (uint64_t)(i + 1u);
            rc = 0;
            pthread_cond_signal(&s->ready);
            break;
        }
    } else rc = -EACCES;
    pthread_mutex_unlock(&s->lock);
    if (rc) { free(owned_path); free(bytes); }
    return rc;
}
static void *worker_main(void *arg) {
    atto_fs_async_service *s = (atto_fs_async_service *)arg;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        job *selected = NULL;
        for (unsigned i = 0; i < s->max_jobs; ++i) {
            job *j = &s->slots[i];
            if (j->state == QUEUED &&
                (!selected || j->sequence < selected->sequence)) selected = j;
        }
        if (!selected) {
            if (s->stopping) break;
            pthread_cond_wait(&s->ready, &s->lock);
            continue;
        }
        selected->state = RUNNING;
        const atto_fs_host_actor owner = selected->owner;
        pthread_mutex_unlock(&s->lock);
        size_t used = 0;
        int rc;
        if (selected->write) {
            rc = atto_fs_broker_write(s->broker, owner.tenant_id, owner.tenant_generation,
                                       selected->path, selected->bytes, selected->length);
        } else {
            rc = atto_fs_broker_read(s->broker, owner.tenant_id, owner.tenant_generation,
                                      selected->path, selected->bytes, selected->length, &used);
        }
        pthread_mutex_lock(&s->lock);
        selected->used = rc ? 0u : used;
        selected->outcome = rc;
        selected->state = DONE;
        if (selected->abandoned) release_job(selected);
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}
int atto_fs_async_create(atto_fs_async_service **out, atto_fs_broker *broker, unsigned max_jobs) {
    if (!out) return -EINVAL;
    *out = NULL;
    if (!broker || max_jobs == 0u || max_jobs > ATTO_FS_ASYNC_MAX_JOBS ||
        broker->policy_layer_count != ATTO_FS_POLICY_LAYER_COUNT) return -EACCES;
    atto_fs_async_service *s = (atto_fs_async_service *)calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    s->broker = broker; s->max_jobs = max_jobs;
    s->admitted_tenant = broker->tenant_id;
    s->admitted_generation = broker->generation;
    int rc = pthread_mutex_init(&s->lock, NULL);
    if (rc) { free(s); return -rc; }
    rc = pthread_cond_init(&s->ready, NULL);
    if (rc) { pthread_mutex_destroy(&s->lock); free(s); return -rc; }
    rc = pthread_create(&s->worker, NULL, worker_main, s);
    if (rc) { pthread_cond_destroy(&s->ready); pthread_mutex_destroy(&s->lock); free(s); return -rc; }
    *out = s;
    return 0;
}
void atto_fs_async_destroy(atto_fs_async_service *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->stopping = 1;
    for (unsigned i = 0; i < s->max_jobs; ++i) {
        job *j = &s->slots[i];
        if (j->state == QUEUED) release_job(j);
        else if (j->state == RUNNING) j->abandoned = 1;
    }
    pthread_cond_broadcast(&s->ready);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->worker, NULL);
    for (unsigned i = 0; i < s->max_jobs; ++i) release_job(&s->slots[i]);
    pthread_cond_destroy(&s->ready);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
int atto_fs_async_submit_read(atto_fs_async_service *s, const atto_fs_host_actor *actor,
                              const char *path, size_t maximum_bytes, uint64_t *ticket) {
    return enqueue(s, actor, path, NULL, maximum_bytes, 0, ticket);
}
int atto_fs_async_submit_write(atto_fs_async_service *s, const atto_fs_host_actor *actor,
                               const char *path, const uint8_t *data, size_t length, uint64_t *ticket) {
    return enqueue(s, actor, path, data, length, 1, ticket);
}
int atto_fs_async_poll(atto_fs_async_service *s, const atto_fs_host_actor *actor,
                       uint64_t ticket, uint8_t *output, size_t capacity, size_t *bytes_read) {
    if (bytes_read) *bytes_read = 0u;
    if (!s || !actor || !bytes_read || (!output && capacity) || capacity > ATTO_FS_MAX_BYTES)
        return -EINVAL;
    /* This function is entered from a guest host-call. Never park its
     * carrier while the worker is finishing another ticket. */
    const int lock_rc = pthread_mutex_trylock(&s->lock);
    if (lock_rc) return lock_rc == EBUSY ? -EAGAIN : -lock_rc;
    job *j = find_ticket(s, ticket);
    int rc;
    if (!j) rc = -ESTALE;
    else if (!same_owner(&j->owner, actor)) rc = -EACCES;
    else if (j->state != DONE) rc = -EAGAIN;
    else {
        /* Running writes may already have committed. Never report a
         * definitive safe cancellation when the outcome is indeterminate. */
        rc = j->cancelled_running_write ? -EINPROGRESS :
             (j->cancelled ? -ECANCELED : j->outcome);
        if (rc == 0 && !j->write) {
            if ((!output && j->used) || j->used > capacity) rc = -EMSGSIZE;
            else rc = atto_fs_broker_publish_read(s->broker,
                         actor->tenant_id, actor->tenant_generation,
                         j->path, j->bytes, j->used, output, capacity);
            if (rc == 0) *bytes_read = j->used;
        }
        /* A worker may hold the broker gate for filesystem I/O or revoke.
         * A delayed publication is still PENDING, NOT consumed: the caller
         * can retry this ticket without blocking a guest carrier. */
        if (rc != -EAGAIN) release_job(j);
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
int atto_fs_async_cancel(atto_fs_async_service *s, const atto_fs_host_actor *actor,
                         uint64_t ticket) {
    if (!s || !actor) return -EINVAL;
    /* Cancellation itself must not park the carrier on worker lock. */
    const int lock_rc = pthread_mutex_trylock(&s->lock);
    if (lock_rc) return lock_rc == EBUSY ? -EAGAIN : -lock_rc;
    job *j = find_ticket(s, ticket);
    int rc;
    if (!j) rc = -ESTALE;
    else if (!same_owner(&j->owner, actor)) rc = -EACCES;
    else if (j->state == DONE) rc = -EALREADY; /* Already completed; poll it. */
    else {
        j->cancelled = 1;
        if (j->state == QUEUED) { j->state = DONE; j->outcome = -ECANCELED; }
        else if (j->state == RUNNING && j->write) j->cancelled_running_write = 1;
        rc = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
int atto_fs_async_forget_actor(atto_fs_async_service *s, const atto_fs_host_actor *actor) {
    if (!s || !actor) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    for (unsigned i = 0; i < s->max_jobs; ++i) {
        job *j = &s->slots[i];
        if (j->state == FREE || !same_owner(&j->owner, actor)) continue;
        if (j->state == RUNNING) { j->cancelled = 1; j->abandoned = 1; }
        else release_job(j);
    }
    pthread_mutex_unlock(&s->lock);
    return 0;
}
