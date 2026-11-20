/* Per-core RT domain: contract + stress.
 *
 * Stress: external producers flood actors spread over every core; each
 * handler forwards a share of its messages to an actor on another core (from
 * the handler, through its own core sender), and a producer mixes in CONTROL.
 * Checks:
 *   - every accepted message runs exactly once (independent counts),
 *   - per (sender, actor) DATA arrives in send order,
 *   - an actor never runs on two threads at once, and always on its core,
 *   - CONTROL queued behind DATA for the same core runs first,
 *   - stop() drains forwarded mail that handlers sent while draining.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeRtDomain.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "native rt domain test failed: %s\n", what);
        abort();
    }
}

#define CORES 4
#define ACTORS 64
#define PRODUCERS 3
#define SENDS 40000
#define FORWARD_TAG (1ull << 63)

typedef struct {
    OresRtDomain* domain;
    uint32_t placement[ACTORS];
    _Atomic unsigned core_owner_phase[CORES]; /* 0=unclaimed, 1=initializing, 2=published */
    pthread_t core_thread_store[CORES];
    atomic_int inside[ACTORS];
    uint64_t last_seq[ACTORS][PRODUCERS]; /* touched only on the actor's core */
    _Atomic uint64_t data_runs, forward_runs, control_runs, forwards_sent;
    _Atomic uint64_t forward_full;
    atomic_int errors;
} Stress;

static void note_core(Stress* st, uint32_t actor) {
    const uint32_t core = st->placement[actor];
    unsigned expected = 0;
    /* Claim the slot BEFORE writing pthread_t, but publish the initialized
     * identity only after a release store. Losing readers must acquire phase
     * 2 before accessing the non-atomic pthread_t. */
    if (atomic_compare_exchange_strong_explicit(&st->core_owner_phase[core],
            &expected, 1u, memory_order_acq_rel, memory_order_acquire)) {
        st->core_thread_store[core] = pthread_self();
        atomic_store_explicit(&st->core_owner_phase[core], 2u, memory_order_release);
        return;
    }
    unsigned phase = expected;
    for (unsigned spins = 0; phase == 1u && spins < 100000u; ++spins) {
        sched_yield();
        phase = atomic_load_explicit(&st->core_owner_phase[core], memory_order_acquire);
    }
    if (phase != 2u || !pthread_equal(st->core_thread_store[core], pthread_self()))
        atomic_fetch_add(&st->errors, 1); /* actor ran off its core's carrier */
}

/* Deliberately make two different carriers race to establish one core's
 * owner. Only the winning carrier may publish pthread_t; the losing carrier
 * must wait for release/acquire publication before comparing. */
typedef struct { Stress* st; atomic_int* ready; atomic_int* go; } OwnerProbe;
static void* probe_note_core(void* raw) {
    OwnerProbe* p = raw;
    atomic_fetch_add(p->ready, 1);
    while (!atomic_load(p->go)) sched_yield();
    note_core(p->st, 0);
    note_core(p->st, 0);
    return NULL;
}
static void owner_publication_race_test(void) {
    for (int round = 0; round < 256; ++round) {
        Stress st = {0};
        atomic_int ready = 0, go = 0;
        OwnerProbe p = {&st, &ready, &go};
        pthread_t a, b;
        check(pthread_create(&a, NULL, probe_note_core, &p) == 0, "owner thread A");
        check(pthread_create(&b, NULL, probe_note_core, &p) == 0, "owner thread B");
        while (atomic_load(&ready) < 2) sched_yield();
        atomic_store(&go, 1);
        pthread_join(a, NULL);
        pthread_join(b, NULL);
        check(atomic_load(&st.core_owner_phase[0]) == 2u, "owner identity fully published");
        check(atomic_load(&st.errors) == 2, "wrong-core thread rejected twice");
    }
    puts("owner publication adversary: OK");
}

static void on_data(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    Stress* st = ctx;
    if (atomic_fetch_add(&st->inside[actor], 1) != 0) atomic_fetch_add(&st->errors, 1);
    if (payload & FORWARD_TAG) {
        atomic_fetch_add(&st->forward_runs, 1);
    } else {
        const uint32_t producer = (uint32_t)(payload >> 32) & 0xff;
        const uint64_t seq = payload & 0xffffffffu;
        if (producer >= PRODUCERS || seq <= st->last_seq[actor][producer])
            atomic_fetch_add(&st->errors, 1); /* FIFO violated */
        else
            st->last_seq[actor][producer] = seq;
        atomic_fetch_add(&st->data_runs, 1);
        /* Forward every 4th message to actors on the next two cores. Two
         * sends per turn matter: stop() must not declare quiescence between
         * them (the second would then be rejected as CLOSED). */
        if ((seq & 3) == 0) {
            for (uint32_t hop = 1; hop <= 2; ++hop) {
                const uint32_t to = (actor + hop * (ACTORS / CORES)) % ACTORS;
                OresRtSendStatus s = ores_rt_send(self, to, FORWARD_TAG | seq);
                if (s == ORES_RT_SEND_OK) atomic_fetch_add(&st->forwards_sent, 1);
                else if (s == ORES_RT_SEND_FULL) atomic_fetch_add(&st->forward_full, 1);
                else atomic_fetch_add(&st->errors, 1);
            }
        }
    }
    note_core(st, actor);
    atomic_fetch_sub(&st->inside[actor], 1);
}

static void on_control(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)self;
    Stress* st = ctx;
    if (payload != 7) atomic_fetch_add(&st->errors, 1);
    if (atomic_fetch_add(&st->inside[actor], 1) != 0) atomic_fetch_add(&st->errors, 1);
    atomic_fetch_add(&st->control_runs, 1);
    note_core(st, actor);
    atomic_fetch_sub(&st->inside[actor], 1);
}

typedef struct { Stress* st; uint32_t id; uint64_t controls; } ProducerArg;

static void* producer(void* raw) {
    ProducerArg* a = raw;
    OresRtSender* s = ores_rt_domain_external_sender(a->st->domain, a->id);
    uint64_t seq[ACTORS] = {0};
    uint32_t rng = 0x9e3779b9u * (a->id + 1);
    for (uint32_t i = 0; i < SENDS; ++i) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const uint32_t actor = rng % ACTORS;
        const uint64_t payload = ((uint64_t)a->id << 32) | (seq[actor] + 1);
        OresRtSendStatus st;
        while ((st = ores_rt_send(s, actor, payload)) == ORES_RT_SEND_FULL) sched_yield();
        check(st == ORES_RT_SEND_OK, "producer send");
        ++seq[actor];
        if (a->id == 0 && (i % 64) == 0) {
            while ((st = ores_rt_send_control(s, rng % ACTORS, 7)) == ORES_RT_SEND_FULL)
                sched_yield();
            check(st == ORES_RT_SEND_OK, "control send");
            ++a->controls;
        }
    }
    return NULL;
}

static void stress(uint32_t spin_ns) {
    Stress* st = calloc(1, sizeof(*st));
    check(st != NULL, "alloc");
    for (uint32_t a = 0; a < ACTORS; ++a) st->placement[a] = a % CORES;
    OresRtDomainOptions o = {
        .core_count = CORES, .actor_count = ACTORS, .actor_core = st->placement,
        .external_senders = PRODUCERS, .ring_capacity = 256, .idle_spin_ns = spin_ns,
        .measure_turns = 1, .on_data = on_data, .on_control = on_control, .ctx = st,
    };
    st->domain = ores_rt_domain_open(&o);
    check(st->domain != NULL, "open");
    check(ores_rt_domain_start(st->domain), "start");

    pthread_t t[PRODUCERS];
    ProducerArg args[PRODUCERS];
    for (uint32_t p = 0; p < PRODUCERS; ++p) {
        args[p] = (ProducerArg){st, p, 0};
        check(pthread_create(&t[p], NULL, producer, &args[p]) == 0, "create");
    }
    for (uint32_t p = 0; p < PRODUCERS; ++p) pthread_join(t[p], NULL);
    check(ores_rt_domain_stop(st->domain), "stop");

    uint64_t external = 0, processed = 0, controls = 0;
    for (uint32_t p = 0; p < PRODUCERS; ++p) {
        external += ores_rt_sender_accepted(ores_rt_domain_external_sender(st->domain, p));
        controls += args[p].controls;
    }
    for (uint32_t c = 0; c < CORES; ++c) {
        OresRtCoreStats cs;
        check(ores_rt_domain_core_stats(st->domain, c, &cs), "stats");
        processed += cs.processed;
    }
    check(external == (uint64_t)PRODUCERS * SENDS + controls, "external accepted count");
    check(atomic_load(&st->data_runs) == (uint64_t)PRODUCERS * SENDS, "every DATA ran once");
    check(atomic_load(&st->control_runs) == controls, "every CONTROL ran once");
    check(atomic_load(&st->forward_runs) == atomic_load(&st->forwards_sent),
          "every forwarded message ran once (including during drain)");
    check(processed == external + atomic_load(&st->forwards_sent), "core processed total");
    check(atomic_load(&st->errors) == 0, "exclusivity / placement / FIFO");
    printf("spin_ns=%u data=%llu forwarded=%llu control=%llu forward_full=%llu: OK\n",
           spin_ns, (unsigned long long)atomic_load(&st->data_runs),
           (unsigned long long)atomic_load(&st->forwards_sent),
           (unsigned long long)controls,
           (unsigned long long)atomic_load(&st->forward_full));
    ores_rt_domain_destroy(st->domain);
    free(st);
}

/* CONTROL sent after DATA to the same core must still run first, as long as
 * both are queued before the core looks. A blocker DATA turn holds the core
 * while we queue them. */
typedef struct {
    atomic_int blocker_in, release;
    int order[8];
    atomic_int count;
} Prio;
static void prio_data(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)self; (void)actor;
    Prio* p = ctx;
    if (payload == 0) {
        atomic_store(&p->blocker_in, 1);
        while (!atomic_load(&p->release)) sched_yield();
        return;
    }
    p->order[atomic_fetch_add(&p->count, 1)] = (int)payload;
}
static void prio_control(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)self; (void)actor;
    Prio* p = ctx;
    p->order[atomic_fetch_add(&p->count, 1)] = 100 + (int)payload;
}
static void control_priority(void) {
    Prio p;
    memset(&p, 0, sizeof(p));
    uint32_t placement[1] = {0};
    OresRtDomainOptions o = {
        .core_count = 1, .actor_count = 1, .actor_core = placement,
        .external_senders = 1, .ring_capacity = 16, .on_data = prio_data,
        .on_control = prio_control, .ctx = &p,
    };
    OresRtDomain* d = ores_rt_domain_open(&o);
    check(d && ores_rt_domain_start(d), "open/start");
    OresRtSender* s = ores_rt_domain_external_sender(d, 0);
    check(ores_rt_send(s, 0, 0) == ORES_RT_SEND_OK, "blocker");
    while (!atomic_load(&p.blocker_in)) sched_yield();
    check(ores_rt_send(s, 0, 1) == ORES_RT_SEND_OK, "data 1");
    check(ores_rt_send(s, 0, 2) == ORES_RT_SEND_OK, "data 2");
    check(ores_rt_send_control(s, 0, 1) == ORES_RT_SEND_OK, "control");
    atomic_store(&p.release, 1);
    check(ores_rt_domain_stop(d), "stop");
    check(atomic_load(&p.count) == 3, "three turns");
    check(p.order[0] == 101 && p.order[1] == 1 && p.order[2] == 2,
          "CONTROL runs before already-queued DATA");
    ores_rt_domain_destroy(d);
    puts("control priority: OK");
}

/* stop() must not see "nothing pending" while a handler is between two
 * sends: A sends to B, waits until B has RUN, then sends to C. If senders
 * counted a message only after publishing it, B's turn could balance the
 * books while A is still running, stop() would close the domain, and A's
 * second send would be rejected. */
typedef struct {
    atomic_int b_ran, ran[3];
    atomic_int second_status;
} Drain;
static void drain_data(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)payload;
    Drain* d = ctx;
    atomic_fetch_add(&d->ran[actor], 1);
    if (actor == 0) {
        check(ores_rt_send(self, 1, 0) == ORES_RT_SEND_OK, "A -> B");
        while (!atomic_load(&d->b_ran)) sched_yield();
        atomic_store(&d->second_status, (int)ores_rt_send(self, 2, 0));
    } else if (actor == 1) {
        atomic_store(&d->b_ran, 1);
    }
}
static void drain_multi_send(void) {
    for (int round = 0; round < 200; ++round) {
        Drain dr;
        memset(&dr, 0, sizeof(dr));
        atomic_store(&dr.second_status, -1);
        uint32_t placement[3] = {0, 1, 1};
        OresRtDomainOptions o = {
            .core_count = 2, .actor_count = 3, .actor_core = placement,
            .external_senders = 1, .ring_capacity = 8, .on_data = drain_data,
            .ctx = &dr,
        };
        OresRtDomain* d = ores_rt_domain_open(&o);
        check(d && ores_rt_domain_start(d), "open/start");
        check(ores_rt_send(ores_rt_domain_external_sender(d, 0), 0, 0) == ORES_RT_SEND_OK,
              "kick");
        check(ores_rt_domain_stop(d), "stop");
        check(atomic_load(&dr.second_status) == ORES_RT_SEND_OK,
              "second send from a running handler was accepted");
        check(atomic_load(&dr.ran[0]) == 1 && atomic_load(&dr.ran[1]) == 1
              && atomic_load(&dr.ran[2]) == 1, "all three turns ran before stop returned");
        ores_rt_domain_destroy(d);
    }
    puts("drain with multi-send handler: OK");
}

/* Lost-wakeup probe: each message is sent just as the core runs out of work
 * and heads for the park path, and the next one waits for it. A lost wakeup
 * leaves exactly one message stranded, so a stalled handoff is a failure. */
static void count_data(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)self; (void)actor; (void)payload;
    atomic_fetch_add((_Atomic uint64_t*)ctx, 1);
}
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void park_handoff(uint32_t spin_ns) {
    _Atomic uint64_t done = 0;
    uint32_t placement[1] = {0};
    OresRtDomainOptions o = {
        .core_count = 1, .actor_count = 1, .actor_core = placement,
        .external_senders = 1, .ring_capacity = 4, .idle_spin_ns = spin_ns,
        .on_data = count_data, .ctx = (void*)&done,
    };
    OresRtDomain* d = ores_rt_domain_open(&o);
    check(d && ores_rt_domain_start(d), "open/start");
    OresRtSender* s = ores_rt_domain_external_sender(d, 0);
    const uint64_t rounds = 50000;
    for (uint64_t i = 0; i < rounds; ++i) {
        check(ores_rt_send(s, 0, i) == ORES_RT_SEND_OK, "handoff send");
        const uint64_t give_up = mono_ns() + 1000000000ull; /* 1 s */
        while (atomic_load(&done) != i + 1) {
            if (mono_ns() > give_up) check(0, "lost wakeup: message stranded on a parked core");
            sched_yield();
        }
    }
    OresRtCoreStats cs;
    check(ores_rt_domain_stop(d) && ores_rt_domain_core_stats(d, 0, &cs), "stop/stats");
    printf("park handoff spin_ns=%u: %llu handoffs, %llu parks: OK\n", spin_ns,
           (unsigned long long)rounds, (unsigned long long)cs.parks);
    ores_rt_domain_destroy(d);
}

/* ---- hardening ---------------------------------------------------------- */
typedef struct { atomic_int release, entered; _Atomic uint64_t runs; } Stuck;
static void stuck_data(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)self; (void)actor;
    Stuck* s = ctx;
    if (payload == 1) {
        atomic_store(&s->entered, 1);
        while (!atomic_load(&s->release)) sched_yield();
    }
    atomic_fetch_add(&s->runs, 1);
}

/* stop_timed gives up on a stuck handler instead of hanging, leaves the
 * domain running, and a later stop succeeds. The stuck turn also shows up as
 * a budget overrun. */
static void timed_stop_and_budget(void) {
    Stuck s;
    memset(&s, 0, sizeof(s));
    uint32_t placement[1] = {0};
    OresRtDomainOptions o = {
        .core_count = 1, .actor_count = 1, .actor_core = placement,
        .external_senders = 1, .ring_capacity = 8, .turn_budget_ns = 1000000, /* 1 ms */
        .on_data = stuck_data, .ctx = &s,
    };
    OresRtDomain* d = ores_rt_domain_open(&o);
    check(d && ores_rt_domain_start(d), "open/start");
    OresRtSender* snd = ores_rt_domain_external_sender(d, 0);
    check(ores_rt_send(snd, 0, 0) == ORES_RT_SEND_OK, "fast turn");
    check(ores_rt_send(snd, 0, 1) == ORES_RT_SEND_OK, "stuck turn");
    while (!atomic_load(&s.entered)) sched_yield();
    check(ores_rt_domain_stop_timed(d, 20000000) == 0, "stop times out on a stuck handler");
    check(ores_rt_send(snd, 0, 0) == ORES_RT_SEND_OK, "still accepting after a timed-out stop");
    struct timespec hold = {0, 5000000};
    nanosleep(&hold, NULL); /* make the stuck turn exceed its 1 ms budget */
    atomic_store(&s.release, 1);
    check(ores_rt_domain_stop_timed(d, 5000000000ull) == 1, "stop succeeds once unstuck");
    OresRtCoreStats cs;
    check(ores_rt_domain_core_stats(d, 0, &cs), "stats");
    check(atomic_load(&s.runs) == 3 && cs.processed == 3, "all three turns ran");
    check(cs.budget_overruns == 1, "exactly the stuck turn overran its budget");
    check(cs.max_turn_ns >= 5000000, "max turn time recorded");
    check(ores_rt_domain_undelivered(d) == 0, "nothing undelivered");
    check(ores_rt_domain_stop_timed(d, 1) == -1, "stop after stop is invalid");
    ores_rt_domain_destroy(d);
    puts("timed stop + turn budget: OK");
}

/* Ordered handoff of an external sender between threads. */
typedef struct { OresRtSender* s; int status_before, status_after, released; } Handoff;
static void* handoff_second(void* raw) {
    Handoff* h = raw;
    h->status_after = (int)ores_rt_send(h->s, 0, 2);
    h->released = ores_rt_sender_release(h->s);
    return NULL;
}
static void* try_send_unreleased(void* raw) {
    Handoff* h = raw;
    h->status_before = (int)ores_rt_send(h->s, 0, 9);
    return NULL;
}
static void sender_handoff(void) {
    _Atomic uint64_t done = 0;
    uint32_t placement[1] = {0};
    OresRtDomainOptions o = {
        .core_count = 1, .actor_count = 1, .actor_core = placement,
        .external_senders = 1, .ring_capacity = 8, .on_data = count_data, .ctx = (void*)&done,
    };
    OresRtDomain* d = ores_rt_domain_open(&o);
    check(d && ores_rt_domain_start(d), "open/start");
    Handoff h = { .s = ores_rt_domain_external_sender(d, 0) };
    check(ores_rt_send(h.s, 0, 1) == ORES_RT_SEND_OK, "first owner sends");
    pthread_t t;
    check(pthread_create(&t, NULL, try_send_unreleased, &h) == 0, "create");
    pthread_join(t, NULL);
    check(h.status_before == ORES_RT_SEND_INVALID, "other thread refused while owned");
    check(ores_rt_sender_release(h.s), "owner releases");
    check(!ores_rt_sender_release(h.s), "release by a non-owner fails");
    check(pthread_create(&t, NULL, handoff_second, &h) == 0, "create");
    pthread_join(t, NULL);
    check(h.status_after == ORES_RT_SEND_OK && h.released, "new owner sends, then releases");
    check(ores_rt_send(h.s, 0, 3) == ORES_RT_SEND_OK, "original thread can take it back");
    check(ores_rt_domain_stop(d), "stop");
    check(atomic_load(&done) == 3 && ores_rt_sender_accepted(h.s) == 3, "three delivered");
    ores_rt_domain_destroy(d);
    puts("sender handoff: OK");
}

/* Contract violation: a producer keeps sending while stop() runs. Whatever
 * it slips in must be either run or reported as undelivered, never lost. */
typedef struct { OresRtDomain* d; atomic_int go; uint64_t ok, closed; } Violator;
static void* violator(void* raw) {
    Violator* v = raw;
    OresRtSender* s = ores_rt_domain_external_sender(v->d, 0);
    while (atomic_load(&v->go)) {
        OresRtSendStatus st = ores_rt_send(s, 0, 0);
        if (st == ORES_RT_SEND_OK) ++v->ok;
        else if (st == ORES_RT_SEND_CLOSED) { ++v->closed; break; }
    }
    return NULL;
}
static void stop_contract_violation(void) {
    uint64_t total_undelivered = 0;
    for (int round = 0; round < 50; ++round) {
        _Atomic uint64_t done = 0;
        uint32_t placement[1] = {0};
        OresRtDomainOptions o = {
            .core_count = 1, .actor_count = 1, .actor_core = placement,
            .external_senders = 1, .ring_capacity = 64, .on_data = count_data,
            .ctx = (void*)&done,
        };
        Violator v = {0};
        v.d = ores_rt_domain_open(&o);
        check(v.d && ores_rt_domain_start(v.d), "open/start");
        atomic_store(&v.go, 1);
        pthread_t t;
        check(pthread_create(&t, NULL, violator, &v) == 0, "create");
        struct timespec pause = {0, 200000};
        nanosleep(&pause, NULL);
        /* Quiescence may never be reached while the violator floods; bound it. */
        int r, tries = 0;
        while ((r = ores_rt_domain_stop_timed(v.d, 2000000)) == 0)
            if (++tries == 50) atomic_store(&v.go, 0); // let it quiesce
        check(r == 1, "stop");
        atomic_store(&v.go, 0);
        pthread_join(t, NULL);
        const uint64_t undelivered = ores_rt_domain_undelivered(v.d);
        check(v.ok == atomic_load(&done) + undelivered,
              "accepted == ran + reported undelivered");
        total_undelivered += undelivered;
        ores_rt_domain_destroy(v.d);
    }
    printf("stop under contract violation: identity held (undelivered reported %llu): OK\n",
           (unsigned long long)total_undelivered);
}

static void noop(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)ctx; (void)self; (void)actor; (void)payload;
}
static OresRtSender* g_other_core_sender;
static atomic_int g_wrong_thread_status = -1;
static void* use_foreign_sender(void* raw) {
    atomic_store(&g_wrong_thread_status, (int)ores_rt_send(raw, 0, 1));
    return NULL;
}

static void contract(void) {
    uint32_t placement[2] = {0, 1};
    OresRtDomainOptions o = {
        .core_count = 2, .actor_count = 2, .actor_core = placement,
        .external_senders = 1, .ring_capacity = 8, .on_data = noop,
    };
    OresRtDomainOptions bad = o;
    bad.ring_capacity = 6;
    check(!ores_rt_domain_open(&bad), "non-power-of-two ring");
    bad = o; bad.core_count = 0;
    check(!ores_rt_domain_open(&bad), "zero cores");
    uint32_t bad_place[2] = {0, 2};
    bad = o; bad.actor_core = bad_place;
    check(!ores_rt_domain_open(&bad), "actor placed on a missing core");
    bad = o; bad.on_data = NULL;
    check(!ores_rt_domain_open(&bad), "missing handler");
    bad = o; bad.idle_spin_ns = ORES_RT_MAX_IDLE_SPIN_NS + 1;
    check(!ores_rt_domain_open(&bad), "spin cap");

    uint32_t cpus[2] = {0, 1};
    bad = o; bad.cpus = cpus;
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    check(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "getaffinity");
    uint32_t n = 0;
    for (uint32_t c = 0; c < CPU_SETSIZE && n < 2; ++c)
        if (CPU_ISSET(c, &allowed)) cpus[n++] = c;
    uint32_t dup[2] = {cpus[0], cpus[0]};
    OresRtDomainOptions dupo = bad; dupo.cpus = dup;
    check(!ores_rt_domain_open(&dupo), "two cores pinned to one CPU rejected");
    if (n == 2) {
        OresRtDomain* pinned = ores_rt_domain_open(&bad);
        check(pinned && ores_rt_domain_start(pinned), "pinned start");
        check(ores_rt_domain_affinity_observed(pinned), "carriers observed their CPUs");
        check(ores_rt_domain_stop(pinned), "pinned stop");
        ores_rt_domain_destroy(pinned);
    }
#else
    check(!ores_rt_domain_open(&bad), "pinning must fail closed without OS support");
#endif

    OresRtDomain* d = ores_rt_domain_open(&o);
    check(d != NULL, "open");
    OresRtSender* s = ores_rt_domain_external_sender(d, 0);
    check(ores_rt_domain_external_sender(d, 1) == NULL, "sender index bound");
    check(ores_rt_send(s, 0, 1) == ORES_RT_SEND_CLOSED, "send before start");
    check(ores_rt_domain_start(d), "start");
    check(!ores_rt_domain_affinity_observed(d), "unpinned reports no affinity");
    check(ores_rt_send(s, 2, 1) == ORES_RT_SEND_INVALID, "actor out of range");
    check(ores_rt_send_control(s, 0, 1) == ORES_RT_SEND_INVALID, "no CONTROL handler");
    check(ores_rt_send(s, 0, 1) == ORES_RT_SEND_OK, "first use binds owner");
    /* Same external sender from another thread: rejected (SPSC). */
    pthread_t t;
    check(pthread_create(&t, NULL, use_foreign_sender, s) == 0, "create");
    pthread_join(t, NULL);
    check(atomic_load(&g_wrong_thread_status) == ORES_RT_SEND_INVALID,
          "external sender is single-owner");
    (void)g_other_core_sender;
    /* Fill core 1's ring from this thread: FULL, never blocks. */
    int full = 0;
    for (int i = 0; i < 64 && !full; ++i)
        full = ores_rt_send(s, 1, (uint64_t)i) == ORES_RT_SEND_FULL;
    (void)full; /* the core may drain fast enough; FULL is allowed, not required */
    check(ores_rt_domain_stop(d), "stop");
    check(ores_rt_send(s, 0, 1) == ORES_RT_SEND_CLOSED, "send after stop");
    check(!ores_rt_domain_stop(d), "stop twice");
    ores_rt_domain_destroy(d);
    puts("contract: OK");
}

int main(void) {
    owner_publication_race_test();
    contract();
    control_priority();
    drain_multi_send();
    timed_stop_and_budget();
    sender_handoff();
    stop_contract_violation();
    park_handoff(0);
    park_handoff(2000);
    stress(0);
    stress(20000);
    puts("native rt per-core domain: OK");
    return 0;
}
