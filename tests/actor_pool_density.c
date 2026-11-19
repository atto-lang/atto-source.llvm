// EXPERIMENT ONLY: trusted host actor scheduler; no guest ABI, tenant sandbox,
// compiled Atto callbacks, signal containment, supervisor CONTROL or I/O broker.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAIL_CAP 16u
#define CARRIERS 2u
#define FAIL_IF_FALSE(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); \
} } while (0)

static void pthread_ok(int rc) {
    if (rc != 0) {
        fprintf(stderr, "FAIL pthread: %d (%s)\n", rc, strerror(rc));
        abort();
    }
}

typedef struct {
    int64_t *mail;                 // lazy fixed-capacity FIFO
    uint64_t sum;                 // only touched by the actor's leased carrier
    uint32_t head, len, generation;
    unsigned queued : 1;         // protected by pool mutex
    unsigned running : 1;        // protected by pool mutex
    atomic_uint turn_lease;       // assertion of non-reentrant execution
} actor_t;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t wake;
    actor_t *actors;
    uint32_t *ready;
    uint32_t cap, size, head, len, active;
    unsigned closing;
    pthread_t carriers[CARRIERS];
    // Host-test controls, not a public runtime contract.
    atomic_uint pause_slot;
    atomic_int pause_enabled, pause_entered;
    unsigned fail_one_mail_alloc;
} pool_t;

// Called with p->mu held. At most one runnable entry per actor.
static void enqueue(pool_t *p, uint32_t slot) {
    actor_t *a = &p->actors[slot];
    FAIL_IF_FALSE(!a->queued && !a->running && a->len && p->len < p->cap);
    p->ready[((uint64_t)p->head + p->len) % p->cap] = slot;
    ++p->len;
    a->queued = 1;
    pthread_ok(pthread_cond_signal(&p->wake));
}

static void *carrier(void *context) {
    pool_t *p = context;
    for (;;) {
        pthread_ok(pthread_mutex_lock(&p->mu));
        while (!p->len && !(p->closing && !p->active))
            pthread_ok(pthread_cond_wait(&p->wake, &p->mu));
        if (!p->len && p->closing && !p->active) {
            pthread_ok(pthread_mutex_unlock(&p->mu));
            return NULL;
        }
        uint32_t slot = p->ready[p->head];
        p->head = (p->head + 1) % p->cap;
        --p->len;
        actor_t *a = &p->actors[slot];
        FAIL_IF_FALSE(a->queued && !a->running && a->len && a->mail);
        a->queued = 0;
        a->running = 1;
        ++p->active;
        int64_t message = a->mail[a->head];
        a->head = (a->head + 1) % MAIL_CAP;
        --a->len;
        pthread_ok(pthread_mutex_unlock(&p->mu));

        // Explicit pause lets the test deterministically fill a mailbox while
        // an actor has an active turn. Only this test host callback may spin.
        if (slot == atomic_load(&p->pause_slot) && atomic_load(&p->pause_enabled)) {
            atomic_store(&p->pause_entered, 1);
            while (atomic_load(&p->pause_enabled)) sched_yield();
        }
        FAIL_IF_FALSE(atomic_fetch_add(&a->turn_lease, 1) == 0);
        // Slot 1 deliberately uses a rolling hash to test FIFO (not just sum).
        if (slot == 1) a->sum = a->sum * UINT64_C(37) + (uint64_t)message;
        else a->sum += (uint64_t)message;
        FAIL_IF_FALSE(atomic_fetch_sub(&a->turn_lease, 1) == 1);

        pthread_ok(pthread_mutex_lock(&p->mu));
        a->running = 0;
        if (a->len) enqueue(p, slot);
        --p->active;
        if (p->closing) pthread_ok(pthread_cond_broadcast(&p->wake));
        pthread_ok(pthread_mutex_unlock(&p->mu));
    }
}

static int pool_open(pool_t *p, uint32_t capacity) {
    if (!capacity) return -1;
    memset(p, 0, sizeof(*p));
    p->actors = calloc(capacity, sizeof(*p->actors));
    p->ready = calloc(capacity, sizeof(*p->ready));
    if (!p->actors || !p->ready) {
        free(p->actors); free(p->ready); return -1;
    }
    p->cap = capacity;
    atomic_init(&p->pause_slot, UINT32_MAX);
    atomic_init(&p->pause_enabled, 0);
    atomic_init(&p->pause_entered, 0);
    if (pthread_mutex_init(&p->mu, NULL) != 0) goto fail_memory;
    if (pthread_cond_init(&p->wake, NULL) != 0) goto fail_mutex;
    for (unsigned i = 0; i < CARRIERS; ++i) {
        if (pthread_create(&p->carriers[i], NULL, carrier, p) != 0) {
            pthread_ok(pthread_mutex_lock(&p->mu));
            p->closing = 1;
            pthread_ok(pthread_cond_broadcast(&p->wake));
            pthread_ok(pthread_mutex_unlock(&p->mu));
            for (unsigned j = 0; j < i; ++j) pthread_ok(pthread_join(p->carriers[j], NULL));
            pthread_ok(pthread_cond_destroy(&p->wake));
            goto fail_mutex;
        }
    }
    return 0;
fail_mutex:
    pthread_ok(pthread_mutex_destroy(&p->mu));
fail_memory:
    free(p->actors); free(p->ready);
    p->actors = NULL;
    p->ready = NULL;
    return -1;
}

// One-shot capacity: no slot reuse/restart and no generation wrap in prototype.
static uint64_t pool_spawn(pool_t *p) {
    pthread_ok(pthread_mutex_lock(&p->mu));
    uint64_t ref = 0;
    if (!p->closing && p->size < p->cap) {
        uint32_t slot = p->size++;
        p->actors[slot].generation = 1;
        atomic_init(&p->actors[slot].turn_lease, 0);
        ref = (UINT64_C(1) << 32) | (uint64_t)(slot + 1);
    }
    pthread_ok(pthread_mutex_unlock(&p->mu));
    return ref;
}

// Linearization: enqueue under mutex. 0 success, -1 invalid/closed,
// -2 mailbox full, -3 memory failure; failure does not mutate queue state.
static int pool_send(pool_t *p, uint64_t ref, int64_t value) {
    uint32_t index = (uint32_t)ref;
    uint32_t generation = (uint32_t)(ref >> 32);
    if (!index) return -1;
    pthread_ok(pthread_mutex_lock(&p->mu));
    int status = -1;
    if (!p->closing && index <= p->size) {
        actor_t *a = &p->actors[index - 1];
        if (generation == a->generation) {
            if (a->len == MAIL_CAP) status = -2;
            else {
                if (!a->mail) {
                    if (p->fail_one_mail_alloc) {
                        p->fail_one_mail_alloc = 0;
                    } else {
                        a->mail = calloc(MAIL_CAP, sizeof(*a->mail));
                    }
                    if (!a->mail) status = -3;
                }
                if (a->mail) {
                    a->mail[(a->head + a->len) % MAIL_CAP] = value;
                    ++a->len;
                    if (!a->running && !a->queued) enqueue(p, index - 1);
                    status = 0;
                }
            }
        }
    }
    pthread_ok(pthread_mutex_unlock(&p->mu));
    return status;
}

// Caller owns pool lifetime; producer threads must finish before pool_free().
// Closing stops future admissions and drains already accepted DATA turns.
static void pool_close(pool_t *p) {
    pthread_ok(pthread_mutex_lock(&p->mu));
    FAIL_IF_FALSE(!p->closing);
    p->closing = 1;
    pthread_ok(pthread_cond_broadcast(&p->wake));
    pthread_ok(pthread_mutex_unlock(&p->mu));
    for (unsigned i = 0; i < CARRIERS; ++i) pthread_ok(pthread_join(p->carriers[i], NULL));
}

static void pool_free(pool_t *p) {
    FAIL_IF_FALSE(p->closing && !p->active && !p->len);
    for (uint32_t i = 0; i < p->size; ++i) {
        FAIL_IF_FALSE(!p->actors[i].running && !p->actors[i].queued && !p->actors[i].len);
        free(p->actors[i].mail);
    }
    free(p->actors); free(p->ready);
    pthread_ok(pthread_cond_destroy(&p->wake));
    pthread_ok(pthread_mutex_destroy(&p->mu));
}

typedef struct {
    pool_t *pool;
    uint64_t actor;
    int iterations;
    uint64_t accepted;
    unsigned until_closed;
    atomic_int acknowledged;
} producer_t;

static void *producer(void *context) {
    producer_t *arg = context;
    for (int i = 0; arg->until_closed || i < arg->iterations;) {
        int rc = pool_send(arg->pool, arg->actor, 1);
        if (rc == 0) { ++i; ++arg->accepted; atomic_store(&arg->acknowledged, 1); }
        else if (rc == -2) sched_yield();
        else { FAIL_IF_FALSE(arg->until_closed && rc == -1); break; }
    }
    return NULL;
}

static void send_retry(pool_t *p, uint64_t actor, int64_t value) {
    // Avoid a silent endless loop on a broken scheduler.
    for (unsigned attempt = 0; attempt < 10000000u; ++attempt) {
        int rc = pool_send(p, actor, value);
        if (rc == 0) return;
        FAIL_IF_FALSE(rc == -2);
        sched_yield();
    }
    FAIL_IF_FALSE(!"scheduler did not drain a saturated mailbox");
}

static void await_pause(pool_t *p) {
    for (unsigned attempt = 0; attempt < 10000000u; ++attempt) {
        if (atomic_load(&p->pause_entered)) return;
        sched_yield();
    }
    FAIL_IF_FALSE(!"carrier did not enter the deliberately paused turn");
}

static void test_density(void) {
    enum { ACTORS = 100000, PRODUCERS = 4, MESSAGES = 3000, FANOUT = 1024 };
    pool_t p;
    FAIL_IF_FALSE(pool_open(&p, ACTORS) == 0);
    uint64_t first = 0, second = 0, fanout[FANOUT];
    for (unsigned i = 0; i < ACTORS; ++i) {
        uint64_t actor = pool_spawn(&p);
        FAIL_IF_FALSE(actor != 0);
        if (!i) first = actor;
        if (i == 1) second = actor;
        if (i >= 2 && i < 2 + FANOUT) fanout[i - 2] = actor;
    }
    FAIL_IF_FALSE(p.size == ACTORS && p.actors[ACTORS - 1].mail == NULL);
    FAIL_IF_FALSE(pool_spawn(&p) == 0); // exact cap
    FAIL_IF_FALSE(pool_send(&p, 0, 1) == -1);
    FAIL_IF_FALSE(pool_send(&p, first ^ (UINT64_C(1) << 32), 1) == -1);
    FAIL_IF_FALSE(pool_send(&p, (UINT64_C(1) << 32) | (ACTORS + 1u), 1) == -1);

    // Simulated fault injection: failed first allocation leaves no queue entry.
    p.fail_one_mail_alloc = 1; // no concurrent sends at this point
    FAIL_IF_FALSE(pool_send(&p, first, 1) == -3);
    FAIL_IF_FALSE(p.actors[0].mail == NULL && p.actors[0].len == 0);

    pthread_t threads[PRODUCERS];
    producer_t jobs[PRODUCERS];
    for (int i = 0; i < PRODUCERS; ++i) {
        jobs[i] = (producer_t){ .pool = &p, .actor = first, .iterations = MESSAGES };
        atomic_init(&jobs[i].acknowledged, 0);
        pthread_ok(pthread_create(&threads[i], NULL, producer, &jobs[i]));
    }
    for (int i = 0; i < PRODUCERS; ++i) pthread_ok(pthread_join(threads[i], NULL));

    // Deterministic mailbox cap: first turn is already running and held.
    atomic_store(&p.pause_slot, 1);
    atomic_store(&p.pause_enabled, 1);
    send_retry(&p, second, 3);
    await_pause(&p);
    for (int i = 0; i < (int)MAIL_CAP; ++i) FAIL_IF_FALSE(pool_send(&p, second, i + 1) == 0);
    FAIL_IF_FALSE(pool_send(&p, second, 999) == -2);
    atomic_store(&p.pause_enabled, 0);
    for (unsigned i = 0; i < FANOUT; ++i) send_retry(&p, fanout[i], 1);
    pool_close(&p);
    FAIL_IF_FALSE(p.actors[0].sum == (uint64_t)PRODUCERS * MESSAGES);
    uint64_t sequence_hash = 3;
    for (int i = 0; i < (int)MAIL_CAP; ++i) sequence_hash = sequence_hash * UINT64_C(37) + (uint64_t)(i + 1);
    FAIL_IF_FALSE(p.actors[1].sum == sequence_hash);
    for (unsigned i = 0; i < FANOUT; ++i) FAIL_IF_FALSE(p.actors[i + 2].sum == 1);
    FAIL_IF_FALSE(p.actors[ACTORS - 1].mail == NULL);
    FAIL_IF_FALSE(pool_send(&p, first, 1) == -1 && pool_spawn(&p) == 0);
    printf("PASS density: 100000 records, %u carriers, 12000 contended turns, 1024 fanout, FIFO, full/backpressure, alloc-failure; actor_size=%zu nominal_array_bytes=%zu\n",
           CARRIERS, sizeof(actor_t), sizeof(actor_t) * (size_t)ACTORS + sizeof(uint32_t) * (size_t)ACTORS);
    pool_free(&p);
}

static void test_close_race(void) {
    enum { PRODUCERS = 4 };
    pool_t p;
    FAIL_IF_FALSE(pool_open(&p, 8) == 0);
    uint64_t actor = pool_spawn(&p);
    FAIL_IF_FALSE(actor != 0);
    pthread_t threads[PRODUCERS];
    producer_t jobs[PRODUCERS];
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        jobs[i] = (producer_t){ .pool = &p, .actor = actor, .until_closed = 1 };
        atomic_init(&jobs[i].acknowledged, 0);
        pthread_ok(pthread_create(&threads[i], NULL, producer, &jobs[i]));
    }
    // Shutdown must race real producers; they can still be sending while
    // close linearizes. Completion means accepted messages are all processed.
    unsigned started = 0;
    for (unsigned attempt = 0; attempt < 10000000u; ++attempt) {
        started = 0;
        for (unsigned i = 0; i < PRODUCERS; ++i)
            started += (unsigned)atomic_load(&jobs[i].acknowledged);
        if (started == PRODUCERS) break;
        sched_yield();
    }
    FAIL_IF_FALSE(started == PRODUCERS);
    pool_close(&p);
    uint64_t total = 0;
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        pthread_ok(pthread_join(threads[i], NULL));
        total += jobs[i].accepted;
    }
    FAIL_IF_FALSE(p.actors[0].sum == total);
    FAIL_IF_FALSE(pool_send(&p, actor, 1) == -1);
    printf("PASS close-race: %llu admitted before close, exactly drained, sends denied afterward\n",
           (unsigned long long)total);
    pool_free(&p);
}

int main(void) {
    FAIL_IF_FALSE(pool_open(&(pool_t){0}, 0) == -1);
    test_density();
    test_close_race();
    puts("PASS experimental actor scheduler host-only safety checks");
    return 0;
}
