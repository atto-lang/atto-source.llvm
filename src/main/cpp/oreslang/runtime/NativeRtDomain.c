#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* cpu_set_t, pthread_attr_setaffinity_np, sched_getcpu */
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "oreslang/runtime/NativeRtDomain.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- SPSC ring ----------------------------------------------------------
 * Same protocol as BoundedSpscQueue.hpp: exactly one release store publishes
 * an item. Each side caches the other's index so an uncontended push/pop
 * touches only its own cache line. */
typedef struct {
    uint32_t actor;
    uint32_t reserved;
    uint64_t payload;
} RtMsg;

typedef struct {
    _Alignas(64) _Atomic uint64_t tail; /* producer-owned */
    uint64_t head_cache;
    _Alignas(64) _Atomic uint64_t head; /* consumer-owned */
    uint64_t tail_cache;
    _Alignas(64) RtMsg* slots;
    uint64_t mask;
} RtRing;

static int ring_push(RtRing* r, RtMsg m) {
    const uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (t - r->head_cache > r->mask) {
        r->head_cache = atomic_load_explicit(&r->head, memory_order_acquire);
        if (t - r->head_cache > r->mask) return 0;
    }
    r->slots[t & r->mask] = m;
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 1;
}

static int ring_pop(RtRing* r, RtMsg* out) {
    const uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    if (h == r->tail_cache) {
        r->tail_cache = atomic_load_explicit(&r->tail, memory_order_acquire);
        if (h == r->tail_cache) return 0;
    }
    *out = r->slots[h & r->mask];
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 1;
}

/* Consumer-side readiness without consuming (used before parking). */
static int ring_ready(RtRing* r) {
    return atomic_load_explicit(&r->tail, memory_order_acquire)
        != atomic_load_explicit(&r->head, memory_order_relaxed);
}

/* ---- domain ------------------------------------------------------------- */
enum { LANE_DATA = 0, LANE_CONTROL = 1, LANES = 2, DATA_BATCH = 16 };

struct OresRtSender {
    OresRtDomain* domain;
    uint32_t source;          /* row in the ring matrix */
    int is_core;
    _Atomic uint64_t owner;   /* unique id of the owning thread; 0 = unowned */
    _Alignas(64) _Atomic uint64_t accepted;
};

typedef struct {
    _Alignas(64) pthread_t thread;
    OresRtDomain* domain;
    uint32_t index;
    int affinity_observed;
    pthread_mutex_t park_mutex;
    pthread_cond_t park_cond;
    _Alignas(64) _Atomic int sleeping;          /* written by consumer + wakers */
    _Alignas(64) _Atomic uint64_t control_hint; /* bumped per CONTROL send */
    _Alignas(64) _Atomic uint64_t processed;    /* carrier-owned counters */
    _Atomic uint64_t control_processed;
    _Atomic uint64_t parks;
    _Atomic uint64_t max_turn_ns;
    _Atomic uint64_t budget_overruns;
} RtCore;

struct OresRtDomain {
    OresRtDomainOptions opt;
    uint32_t cores, sources;     /* sources = cores + external senders */
    uint32_t* actor_core;
    uint32_t cpus[ORES_RT_MAX_CORES];
    int pinned;
    RtCore* core;
    OresRtSender* senders;       /* [0, cores) core senders, then external */
    RtRing* rings;               /* [lane][source][core] */
    RtMsg* slots;
    _Atomic int accepting;       /* sends allowed */
    _Atomic int stop;            /* carriers exit (set only once quiescent) */
    int started, stopped;
    uint64_t undelivered;        /* accepted but never run (set by stop) */
    pthread_mutex_t start_mutex;
    pthread_cond_t start_cond;
    uint32_t checked_in;
};

/* Thread identity for sender ownership. Never reused: a TLS address can be
 * recycled by a later thread, which would let it inherit an exited owner's
 * sender with no ordering against that owner's last push. */
static _Atomic uint64_t next_thread_id = 1;
static _Thread_local uint64_t thread_id;
static uint64_t self_id(void) {
    if (!thread_id) thread_id = atomic_fetch_add_explicit(&next_thread_id, 1,
                                                          memory_order_relaxed);
    return thread_id;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

static RtRing* ring_at(OresRtDomain* d, int lane, uint32_t source, uint32_t core) {
    return &d->rings[((size_t)lane * d->sources + source) * d->cores + core];
}

static int core_has_work(OresRtDomain* d, uint32_t core) {
    for (int lane = 0; lane < LANES; ++lane)
        for (uint32_t s = 0; s < d->sources; ++s)
            if (ring_ready(ring_at(d, lane, s, core))) return 1;
    return 0;
}

static void wake_core(RtCore* c) {
    pthread_mutex_lock(&c->park_mutex);
    atomic_store_explicit(&c->sleeping, 0, memory_order_relaxed);
    pthread_cond_signal(&c->park_cond);
    pthread_mutex_unlock(&c->park_mutex);
}

/* The park mutex is the only lock a SCHED_FIFO carrier ever takes. A
 * low-priority sender holds it briefly to wake the carrier; with priority
 * inheritance that sender cannot be preempted indefinitely while holding it. */
static void init_park_mutex(pthread_mutex_t* m) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT >= 0
        if (pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) == 0
                && pthread_mutex_init(m, &attr) == 0) {
            pthread_mutexattr_destroy(&attr);
            return;
        }
#endif
        pthread_mutexattr_destroy(&attr);
    }
    pthread_mutex_init(m, NULL);
}

OresRtDomain* ores_rt_domain_open(const OresRtDomainOptions* o) {
    if (!o || !o->on_data || !o->actor_core
            || o->core_count == 0 || o->core_count > ORES_RT_MAX_CORES
            || o->actor_count == 0 || o->actor_count > ORES_RT_MAX_ACTORS
            || o->external_senders > ORES_RT_MAX_EXTERNAL
            || o->ring_capacity < 2 || o->ring_capacity > ORES_RT_MAX_RING
            || (o->ring_capacity & (o->ring_capacity - 1)) != 0
            || o->idle_spin_ns > ORES_RT_MAX_IDLE_SPIN_NS)
        return NULL;
    for (uint32_t a = 0; a < o->actor_count; ++a)
        if (o->actor_core[a] >= o->core_count) return NULL;
    if (o->sched_fifo_priority) {
        const int lo = sched_get_priority_min(SCHED_FIFO);
        const int hi = sched_get_priority_max(SCHED_FIFO);
        if (lo < 0 || o->sched_fifo_priority < lo || o->sched_fifo_priority > hi)
            return NULL;
    }
#if !defined(__linux__)
    if (o->cpus) return NULL; /* no hard affinity here: fail closed */
#else
    if (o->cpus) {
        cpu_set_t allowed;
        CPU_ZERO(&allowed);
        if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) return NULL;
        for (uint32_t i = 0; i < o->core_count; ++i)
            if (o->cpus[i] >= CPU_SETSIZE || !CPU_ISSET(o->cpus[i], &allowed))
                return NULL;
    }
#endif
    /* Two carriers on one CPU defeat the point of pinning: under SCHED_FIFO
     * with idle spin, one can starve the other outright. */
    if (o->cpus)
        for (uint32_t i = 0; i < o->core_count; ++i)
            for (uint32_t j = i + 1; j < o->core_count; ++j)
                if (o->cpus[i] == o->cpus[j]) return NULL;
    {
        const size_t rings = (size_t)2 * (o->core_count + o->external_senders) * o->core_count;
        if ((size_t)o->ring_capacity > SIZE_MAX / sizeof(RtMsg) / rings - 1)
            return NULL; /* slot array size would overflow size_t */
    }

    OresRtDomain* d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->opt = *o;
    d->cores = o->core_count;
    d->sources = o->core_count + o->external_senders;
    if (o->cpus) {
        memcpy(d->cpus, o->cpus, o->core_count * sizeof(uint32_t));
        d->pinned = 1;
    }
    d->opt.cpus = NULL; /* caller arrays are copied, never retained */
    d->opt.actor_core = NULL;

    const size_t rings = (size_t)LANES * d->sources * d->cores;
    const size_t slots = rings * o->ring_capacity;
    d->actor_core = malloc(o->actor_count * sizeof(uint32_t));
    d->core = aligned_alloc(64, ((d->cores * sizeof(RtCore) + 63) / 64) * 64);
    d->senders = aligned_alloc(64, ((d->sources * sizeof(OresRtSender) + 63) / 64) * 64);
    d->rings = aligned_alloc(64, ((rings * sizeof(RtRing) + 63) / 64) * 64);
    d->slots = aligned_alloc(64, ((slots * sizeof(RtMsg) + 63) / 64) * 64);
    if (!d->actor_core || !d->core || !d->senders || !d->rings || !d->slots) {
        free(d->actor_core); free(d->core); free(d->senders);
        free(d->rings); free(d->slots); free(d);
        return NULL;
    }
    memcpy(d->actor_core, o->actor_core, o->actor_count * sizeof(uint32_t));
    /* Touch every page now so the RT window never takes a first-use fault. */
    memset(d->core, 0, d->cores * sizeof(RtCore));
    memset(d->senders, 0, d->sources * sizeof(OresRtSender));
    memset(d->rings, 0, rings * sizeof(RtRing));
    memset(d->slots, 0, slots * sizeof(RtMsg));
    for (size_t r = 0; r < rings; ++r) {
        d->rings[r].slots = d->slots + r * o->ring_capacity;
        d->rings[r].mask = o->ring_capacity - 1;
        atomic_init(&d->rings[r].head, 0);
        atomic_init(&d->rings[r].tail, 0);
    }
    for (uint32_t s = 0; s < d->sources; ++s) {
        d->senders[s].domain = d;
        d->senders[s].source = s;
        d->senders[s].is_core = s < d->cores;
        atomic_init(&d->senders[s].owner, 0);
        atomic_init(&d->senders[s].accepted, 0);
    }
    for (uint32_t c = 0; c < d->cores; ++c) {
        RtCore* core = &d->core[c];
        core->domain = d;
        core->index = c;
        init_park_mutex(&core->park_mutex);
        pthread_cond_init(&core->park_cond, NULL);
        atomic_init(&core->sleeping, 0);
        atomic_init(&core->control_hint, 0);
        atomic_init(&core->processed, 0);
        atomic_init(&core->control_processed, 0);
        atomic_init(&core->parks, 0);
        atomic_init(&core->max_turn_ns, 0);
        atomic_init(&core->budget_overruns, 0);
    }
    pthread_mutex_init(&d->start_mutex, NULL);
    pthread_cond_init(&d->start_cond, NULL);
    atomic_init(&d->accepting, 0);
    atomic_init(&d->stop, 0);
    return d;
}

static int observe_affinity(OresRtDomain* d, uint32_t core) {
#if defined(__linux__)
    if (!d->pinned) return 0;
    cpu_set_t actual;
    CPU_ZERO(&actual);
    if (pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual) != 0) return 0;
    return CPU_COUNT(&actual) == 1 && CPU_ISSET(d->cpus[core], &actual)
        && sched_getcpu() == (int)d->cpus[core];
#else
    (void)d; (void)core;
    return 0;
#endif
}

static void run_turn(OresRtDomain* d, RtCore* c, OresRtHandlerFn fn,
                     const RtMsg* m, int control) {
    OresRtSender* self = &d->senders[c->index];
    if (d->opt.measure_turns || d->opt.turn_budget_ns) {
        const uint64_t t0 = now_ns();
        fn(d->opt.ctx, self, m->actor, m->payload);
        const uint64_t dt = now_ns() - t0;
        if (dt > atomic_load_explicit(&c->max_turn_ns, memory_order_relaxed))
            atomic_store_explicit(&c->max_turn_ns, dt, memory_order_relaxed);
        if (d->opt.turn_budget_ns && dt > d->opt.turn_budget_ns)
            atomic_store_explicit(&c->budget_overruns,
                atomic_load_explicit(&c->budget_overruns, memory_order_relaxed) + 1,
                memory_order_relaxed);
    } else {
        fn(d->opt.ctx, self, m->actor, m->payload);
    }
    if (control)
        atomic_store_explicit(&c->control_processed,
            atomic_load_explicit(&c->control_processed, memory_order_relaxed) + 1,
            memory_order_relaxed);
    /* Release: stop() pairs this with its acquire read of all counters. */
    atomic_store_explicit(&c->processed,
        atomic_load_explicit(&c->processed, memory_order_relaxed) + 1,
        memory_order_release);
}

static size_t drain_control(OresRtDomain* d, RtCore* c, uint64_t* seen_hint) {
    size_t n = 0;
    RtMsg m;
    *seen_hint = atomic_load_explicit(&c->control_hint, memory_order_acquire);
    for (uint32_t s = 0; s < d->sources; ++s) {
        RtRing* r = ring_at(d, LANE_CONTROL, s, c->index);
        while (ring_pop(r, &m)) {
            run_turn(d, c, d->opt.on_control, &m, 1);
            ++n;
        }
    }
    return n;
}

static void* rt_carrier(void* raw) {
    RtCore* c = raw;
    OresRtDomain* d = c->domain;
    atomic_store_explicit(&d->senders[c->index].owner, self_id(),
                          memory_order_relaxed);
    c->affinity_observed = observe_affinity(d, c->index);
    pthread_mutex_lock(&d->start_mutex);
    ++d->checked_in;
    pthread_cond_broadcast(&d->start_cond);
    pthread_mutex_unlock(&d->start_mutex);

    uint64_t seen_hint = 0;
    uint32_t rr = 0;
    for (;;) {
        size_t n = d->opt.on_control ? drain_control(d, c, &seen_hint) : 0;
        RtMsg m;
        for (uint32_t k = 0; k < d->sources; ++k) {
            uint32_t s = rr + k;
            if (s >= d->sources) s -= d->sources;
            RtRing* r = ring_at(d, LANE_DATA, s, c->index);
            for (int b = 0; b < DATA_BATCH && ring_pop(r, &m); ++b) {
                run_turn(d, c, d->opt.on_data, &m, 0);
                ++n;
                /* CONTROL outranks DATA: re-check between DATA turns. */
                if (d->opt.on_control
                        && atomic_load_explicit(&c->control_hint, memory_order_relaxed)
                           != seen_hint)
                    n += drain_control(d, c, &seen_hint);
            }
        }
        if (++rr == d->sources) rr = 0;
        if (n) continue;
        if (atomic_load_explicit(&d->stop, memory_order_acquire)) break;

        if (d->opt.idle_spin_ns) {
            const uint64_t deadline = now_ns() + d->opt.idle_spin_ns;
            int ready = 0;
            for (unsigned i = 1;; ++i) {
                if (core_has_work(d, c->index)
                        || atomic_load_explicit(&d->stop, memory_order_relaxed)) {
                    ready = 1;
                    break;
                }
                cpu_relax();
                if ((i & 15u) == 0 && now_ns() >= deadline) break;
            }
            if (ready) continue;
        }
        /* Park. Dekker with ores_rt_send: we publish `sleeping` then re-check
         * the rings; a producer publishes its item then checks `sleeping`.
         * With both fences seq_cst, at least one side sees the other. */
        atomic_store_explicit(&c->sleeping, 1, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        if (core_has_work(d, c->index)
                || atomic_load_explicit(&d->stop, memory_order_seq_cst)) {
            atomic_store_explicit(&c->sleeping, 0, memory_order_relaxed);
            continue;
        }
        pthread_mutex_lock(&c->park_mutex);
        while (atomic_load_explicit(&c->sleeping, memory_order_relaxed)
                && !atomic_load_explicit(&d->stop, memory_order_relaxed))
            pthread_cond_wait(&c->park_cond, &c->park_mutex);
        pthread_mutex_unlock(&c->park_mutex);
        atomic_store_explicit(&c->parks,
            atomic_load_explicit(&c->parks, memory_order_relaxed) + 1,
            memory_order_relaxed);
    }
    return NULL;
}

static void stop_and_join(OresRtDomain* d, uint32_t created) {
    atomic_store_explicit(&d->stop, 1, memory_order_seq_cst);
    for (uint32_t c = 0; c < created; ++c) wake_core(&d->core[c]);
    for (uint32_t c = 0; c < created; ++c) pthread_join(d->core[c].thread, NULL);
}

int ores_rt_domain_start(OresRtDomain* d) {
    if (!d || d->started) return 0;
    uint32_t created = 0;
    for (; created < d->cores; ++created) {
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) break;
        int rc = 0;
        if (d->opt.sched_fifo_priority) {
            struct sched_param p = { .sched_priority = d->opt.sched_fifo_priority };
            rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
            if (rc == 0) rc = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
            if (rc == 0) rc = pthread_attr_setschedparam(&attr, &p);
        }
#if defined(__linux__)
        if (rc == 0 && d->pinned) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(d->cpus[created], &set);
            rc = pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
        }
#endif
        if (rc == 0)
            rc = pthread_create(&d->core[created].thread, &attr, rt_carrier,
                                &d->core[created]);
        pthread_attr_destroy(&attr);
        if (rc != 0) break;
    }
    if (created != d->cores) {
        /* Fail closed: never run with fewer cores or a weaker policy. */
        stop_and_join(d, created);
        d->stopped = 1;
        return 0;
    }
    pthread_mutex_lock(&d->start_mutex);
    while (d->checked_in < d->cores)
        pthread_cond_wait(&d->start_cond, &d->start_mutex);
    pthread_mutex_unlock(&d->start_mutex);
    d->started = 1;
    atomic_store_explicit(&d->accepting, 1, memory_order_release);
    return 1;
}

int ores_rt_domain_affinity_observed(OresRtDomain* d) {
    if (!d || !d->started || !d->pinned) return 0;
    for (uint32_t c = 0; c < d->cores; ++c)
        if (!d->core[c].affinity_observed) return 0;
    return 1;
}

OresRtSender* ores_rt_domain_external_sender(OresRtDomain* d, uint32_t index) {
    if (!d || index >= d->opt.external_senders) return NULL;
    return &d->senders[d->cores + index];
}

static OresRtSendStatus rt_send(OresRtSender* s, uint32_t actor,
                                uint64_t payload, int lane) {
    if (!s) return ORES_RT_SEND_INVALID;
    OresRtDomain* d = s->domain;
    if (!atomic_load_explicit(&d->accepting, memory_order_acquire))
        return ORES_RT_SEND_CLOSED;
    if (actor >= d->opt.actor_count
            || (lane == LANE_CONTROL && !d->opt.on_control))
        return ORES_RT_SEND_INVALID;
    /* SPSC: one owning thread at a time. Taking an unowned sender acquires
     * the previous owner's release, so its last pushes happen-before ours. */
    const uint64_t me = self_id();
    uint64_t owner = atomic_load_explicit(&s->owner, memory_order_relaxed);
    if (owner != me) {
        if (s->is_core || owner != 0) return ORES_RT_SEND_INVALID;
        if (!atomic_compare_exchange_strong_explicit(&s->owner, &owner, me,
                memory_order_acquire, memory_order_relaxed))
            return ORES_RT_SEND_INVALID;
    }
    const uint32_t core = d->actor_core[actor];
    /* Count before publishing so total processed never exceeds total
     * accepted (stop()'s quiescence proof); undo if the ring is full. */
    const uint64_t accepted = atomic_load_explicit(&s->accepted, memory_order_relaxed);
    atomic_store_explicit(&s->accepted, accepted + 1, memory_order_release);
    if (!ring_push(ring_at(d, lane, s->source, core),
                   (RtMsg){ .actor = actor, .payload = payload })) {
        atomic_store_explicit(&s->accepted, accepted, memory_order_release);
        return ORES_RT_SEND_FULL;
    }
    RtCore* target = &d->core[core];
    if (lane == LANE_CONTROL)
        atomic_fetch_add_explicit(&target->control_hint, 1, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&target->sleeping, memory_order_relaxed))
        wake_core(target);
    return ORES_RT_SEND_OK;
}

OresRtSendStatus ores_rt_send(OresRtSender* s, uint32_t actor, uint64_t payload) {
    return rt_send(s, actor, payload, LANE_DATA);
}

OresRtSendStatus ores_rt_send_control(OresRtSender* s, uint32_t actor, uint64_t payload) {
    return rt_send(s, actor, payload, LANE_CONTROL);
}

int ores_rt_sender_release(OresRtSender* s) {
    if (!s || s->is_core) return 0;
    uint64_t me = self_id();
    return atomic_compare_exchange_strong_explicit(&s->owner, &me, 0,
            memory_order_release, memory_order_relaxed);
}

uint64_t ores_rt_sender_accepted(const OresRtSender* s) {
    return s ? atomic_load_explicit(&((OresRtSender*)s)->accepted, memory_order_acquire) : 0;
}

static void totals(OresRtDomain* d, uint64_t* processed, uint64_t* accepted) {
    uint64_t p = 0, a = 0;
    for (uint32_t c = 0; c < d->cores; ++c)
        p += atomic_load_explicit(&d->core[c].processed, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    for (uint32_t s = 0; s < d->sources; ++s)
        a += atomic_load_explicit(&d->senders[s].accepted, memory_order_acquire);
    *processed = p;
    *accepted = a;
}

int ores_rt_domain_stop(OresRtDomain* d) {
    return ores_rt_domain_stop_timed(d, UINT64_MAX) == 1;
}

int ores_rt_domain_stop_timed(OresRtDomain* d, uint64_t timeout_ns) {
    if (!d || !d->started || d->stopped) return -1;
    for (uint32_t c = 0; c < d->cores; ++c)
        if (pthread_equal(pthread_self(), d->core[c].thread)) return -1;
    const uint64_t start = now_ns();
    const uint64_t deadline = timeout_ns > UINT64_MAX - start ? UINT64_MAX : start + timeout_ns;
    /* Quiescence: read every processed counter, then every accepted
     * counter. Each turn bumps processed only after its handler returns and
     * each send bumps accepted before publishing, so processed <= accepted
     * always, and equality means nothing is queued and no handler is running
     * (a running handler's own message is accepted but not yet processed).
     * External senders are fenced by the caller; handlers cannot run. */
    for (unsigned spins = 0;; ++spins) {
        uint64_t processed, accepted;
        totals(d, &processed, &accepted);
        if (processed == accepted) break;
        if (now_ns() >= deadline) return 0; /* still running; may retry */
        if (spins < 1000) cpu_relax();
        else {
            struct timespec pause = {0, 50000};
            nanosleep(&pause, NULL);
        }
    }
    atomic_store_explicit(&d->accepting, 0, memory_order_seq_cst);
    stop_and_join(d, d->cores);
    d->stopped = 1;
    /* A sender that ignored the "stop external senders first" contract can
     * slip a message in between the quiescence check and closing. It was
     * counted as accepted but no carrier is left to run it: report it. */
    uint64_t processed, accepted;
    totals(d, &processed, &accepted);
    d->undelivered = accepted - processed;
    return 1;
}

uint64_t ores_rt_domain_undelivered(OresRtDomain* d) {
    return d && d->stopped ? d->undelivered : 0;
}

int ores_rt_domain_core_stats(OresRtDomain* d, uint32_t core, OresRtCoreStats* out) {
    if (!d || !out || core >= d->cores) return 0;
    RtCore* c = &d->core[core];
    out->processed = atomic_load_explicit(&c->processed, memory_order_acquire);
    out->control_processed = atomic_load_explicit(&c->control_processed, memory_order_relaxed);
    out->parks = atomic_load_explicit(&c->parks, memory_order_relaxed);
    out->max_turn_ns = atomic_load_explicit(&c->max_turn_ns, memory_order_relaxed);
    out->budget_overruns = atomic_load_explicit(&c->budget_overruns, memory_order_relaxed);
    return 1;
}

void ores_rt_domain_destroy(OresRtDomain* d) {
    if (!d) return;
    if (d->started && !d->stopped) abort(); /* carriers still reference d */
    for (uint32_t c = 0; c < d->cores; ++c) {
        pthread_mutex_destroy(&d->core[c].park_mutex);
        pthread_cond_destroy(&d->core[c].park_cond);
    }
    pthread_mutex_destroy(&d->start_mutex);
    pthread_cond_destroy(&d->start_cond);
    free(d->actor_core);
    free(d->core);
    free(d->senders);
    free(d->rings);
    free(d->slots);
    free(d);
}
