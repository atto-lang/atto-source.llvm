// Experimental trusted actor scheduler: NOT a TenantVM sandbox or guest ABI.
// Build: cc -std=c11 -O2 -Wall -Wextra -Werror -pthread tests/actor_pool_density.c -o /tmp/actor_pool_density
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAIL_CAP 16u
#define CARRIERS 2u

typedef struct {
    int64_t *mail; // lazy; no per-actor queue for never-messaged actors
    uint64_t sum;
    uint32_t head, len, generation;
    unsigned queued : 1;
    unsigned running : 1;
    atomic_uint turn_lease;
} actor_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t wake;
    actor_t *actors;
    uint32_t *ready;
    uint32_t cap, size, head, len, active;
    unsigned closing;
    pthread_t carriers[CARRIERS];
} pool_t;

static void enqueue(pool_t *p, uint32_t slot) {
    actor_t *a = &p->actors[slot];
    assert(!a->queued && !a->running && p->len < p->cap);
    p->ready[(p->head + p->len) % p->cap] = slot;
    ++p->len;
    a->queued = 1;
    pthread_cond_signal(&p->wake);
}

static void *carrier(void *context) {
    pool_t *p = context;
    for (;;) {
        assert(pthread_mutex_lock(&p->mu) == 0);
        while (!p->len && !(p->closing && !p->active))
            assert(pthread_cond_wait(&p->wake, &p->mu) == 0);
        if (!p->len && p->closing && !p->active) {
            assert(pthread_mutex_unlock(&p->mu) == 0);
            return NULL;
        }
        uint32_t slot = p->ready[p->head];
        p->head = (p->head + 1) % p->cap;
        --p->len;
        actor_t *a = &p->actors[slot];
        assert(a->queued && !a->running && a->len);
        a->queued = 0;
        a->running = 1;
        ++p->active;
        int64_t message = a->mail[a->head];
        a->head = (a->head + 1) % MAIL_CAP;
        --a->len;
        assert(pthread_mutex_unlock(&p->mu) == 0);

        // Compiler-generated actor callback would run here with a per-turn
        // authenticated actor context. This host-only example updates a sum.
        assert(atomic_fetch_add(&a->turn_lease, 1) == 0);
        a->sum += (uint64_t)message;
        assert(atomic_fetch_sub(&a->turn_lease, 1) == 1);

        assert(pthread_mutex_lock(&p->mu) == 0);
        a->running = 0;
        if (a->len) enqueue(p, slot);
        --p->active;
        if (p->closing) pthread_cond_broadcast(&p->wake);
        assert(pthread_mutex_unlock(&p->mu) == 0);
    }
}

static int pool_open(pool_t *p, uint32_t capacity) {
    if (!capacity) return -1;
    memset(p, 0, sizeof(*p));
    p->actors = calloc(capacity, sizeof(*p->actors));
    p->ready = calloc(capacity, sizeof(*p->ready));
    if (!p->actors || !p->ready) { free(p->actors); free(p->ready); return -1; }
    p->cap = capacity;
    if (pthread_mutex_init(&p->mu, NULL)) goto fail_memory;
    if (pthread_cond_init(&p->wake, NULL)) goto fail_mutex;
    for (unsigned i = 0; i < CARRIERS; ++i) {
        if (pthread_create(&p->carriers[i], NULL, carrier, p)) {
            assert(pthread_mutex_lock(&p->mu) == 0);
            p->closing = 1;
            pthread_cond_broadcast(&p->wake);
            assert(pthread_mutex_unlock(&p->mu) == 0);
            for (unsigned j = 0; j < i; ++j) assert(!pthread_join(p->carriers[j], NULL));
            pthread_cond_destroy(&p->wake);
            goto fail_mutex;
        }
    }
    return 0;
fail_mutex:
    pthread_mutex_destroy(&p->mu);
fail_memory:
    free(p->actors); free(p->ready);
    return -1;
}

static uint64_t pool_spawn(pool_t *p) {
    assert(pthread_mutex_lock(&p->mu) == 0);
    uint64_t ref = 0;
    if (!p->closing && p->size < p->cap) {
        uint32_t slot = p->size++;
        p->actors[slot].generation = 1;
        atomic_init(&p->actors[slot].turn_lease, 0);
        ref = (UINT64_C(1) << 32) | (uint64_t)(slot + 1);
    }
    assert(pthread_mutex_unlock(&p->mu) == 0);
    return ref;
}

// 0 accepted; -1 invalid/closed, -2 backpressure, -3 allocation failure.
static int pool_send(pool_t *p, uint64_t ref, int64_t value) {
    uint32_t index = (uint32_t)ref;
    uint32_t generation = (uint32_t)(ref >> 32);
    if (!index) return -1;
    assert(pthread_mutex_lock(&p->mu) == 0);
    int status = -1;
    if (!p->closing && index <= p->size) {
        actor_t *a = &p->actors[index - 1];
        if (generation == a->generation) {
            if (a->len == MAIL_CAP) status = -2;
            else {
                if (!a->mail) a->mail = calloc(MAIL_CAP, sizeof(*a->mail));
                if (!a->mail) status = -3;
                else {
                    a->mail[(a->head + a->len) % MAIL_CAP] = value;
                    ++a->len;
                    if (!a->running && !a->queued) enqueue(p, index - 1);
                    status = 0;
                }
            }
        }
    }
    assert(pthread_mutex_unlock(&p->mu) == 0);
    return status;
}

static void pool_close(pool_t *p) {
    assert(pthread_mutex_lock(&p->mu) == 0);
    p->closing = 1;
    pthread_cond_broadcast(&p->wake);
    assert(pthread_mutex_unlock(&p->mu) == 0);
    for (unsigned i = 0; i < CARRIERS; ++i) assert(!pthread_join(p->carriers[i], NULL));
}

static void pool_free(pool_t *p) {
    for (uint32_t i = 0; i < p->size; ++i) free(p->actors[i].mail);
    free(p->actors); free(p->ready);
    pthread_cond_destroy(&p->wake);
    pthread_mutex_destroy(&p->mu);
}

typedef struct { pool_t *pool; uint64_t actor; int iterations; } producer_t;
static void *producer(void *context) {
    producer_t *arg = context;
    for (int i = 0; i < arg->iterations;) {
        int result = pool_send(arg->pool, arg->actor, 1);
        if (!result) ++i;
        else { assert(result == -2); sched_yield(); }
    }
    return NULL;
}

int main(void) {
    enum { ACTORS = 100000, PRODUCERS = 4, MESSAGES = 3000 };
    pool_t p;
    assert(pool_open(&p, ACTORS) == 0);
    uint64_t first = 0;
    for (unsigned i = 0; i < ACTORS; ++i) {
        uint64_t actor = pool_spawn(&p);
        assert(actor != 0);
        if (!i) first = actor;
    }
    assert(p.size == ACTORS);
    assert(p.actors[ACTORS - 1].mail == NULL); // true lazy mailbox
    assert(pool_send(&p, first ^ (UINT64_C(1) << 32), 1) == -1);
    assert(pool_send(&p, 0, 1) == -1);
    pthread_t threads[PRODUCERS];
    producer_t job = { &p, first, MESSAGES };
    for (int i = 0; i < PRODUCERS; ++i)
        assert(!pthread_create(&threads[i], NULL, producer, &job));
    for (int i = 0; i < PRODUCERS; ++i) assert(!pthread_join(threads[i], NULL));
    pool_close(&p);
    assert(p.actors[0].sum == PRODUCERS * MESSAGES);
    assert(pool_send(&p, first, 1) == -1);
    printf("PASS: %u idle actor records, %u pooled carriers, %u FIFO actor turns; sizeof(actor)=%zu, array bytes=%zu (host-only)\n",
           (unsigned)ACTORS, CARRIERS, PRODUCERS * MESSAGES, sizeof(actor_t),
           sizeof(actor_t) * (size_t)ACTORS + sizeof(uint32_t) * (size_t)ACTORS);
    pool_free(&p);
    return 0;
}
