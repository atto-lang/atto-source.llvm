/* Native actor scheduler latency benchmark with an OS/kernel floor.
 *
 * Goal: show how much latency the scheduler itself adds on top of what the
 * kernel and hardware already cost. Each scenario prints one machine-readable
 * line:
 *
 *   scenario=<name> ... p50_ns=.. p99_ns=.. p999_ns=.. max_ns=.. mean_ns=..
 *
 * Floors (no scheduler involved):
 *   floor_condvar  two pthreads ping-pong through mutex + condvar. Every hop
 *                  parks and wakes a thread, so this is the kernel wake cost.
 *   floor_spin     two pthreads ping-pong through one atomic. No kernel at
 *                  all: this is the cache-line transfer cost.
 *
 * Scheduler scenarios:
 *   sched_pingpong two actors hand one message back and forth. Each hop is
 *                  measured from try_send to the start of the receiving
 *                  callback. Actors sit at slot 0 and slot N-1, so with N large
 *                  a scheduler that scans every lane pays for it on each hop.
 *   sched_fanin    P producer threads flood K actors on a scheduler with N
 *                  slots; reports turns per second (throughput, not latency).
 *
 * When sched_pingpong is close to floor_condvar (parked carriers) or close to
 * floor_spin (spinning carriers), the kernel/hardware, not the scheduler, is
 * the bottleneck.
 *
 * Usage: bench [--smoke] [--hops N] [--spin-ns N] [--cpus a,b] [--lock pi|throughput]
 * --smoke runs tiny iteration counts so CTest keeps the binary exercised.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "bench check failed: %s\n", what);
        abort();
    }
}

static uint64_t now_ns(void) {
#if defined(__APPLE__)
    /* CLOCK_MONOTONIC is microsecond-granular on macOS; this one is not. */
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/* --lock throughput: open schedulers without priority inheritance. */
static int g_lock_throughput;

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static void report(const char* scenario, const char* extra,
                   uint64_t* samples, size_t n) {
    check(n > 0, "samples");
    qsort(samples, n, sizeof(*samples), cmp_u64);
    long double sum = 0;
    for (size_t i = 0; i < n; ++i) sum += samples[i];
    size_t p999 = (size_t)((long double)n * 0.999L);
    if (p999 >= n) p999 = n - 1;
    printf("scenario=%s %s samples=%zu p50_ns=%llu p99_ns=%llu p999_ns=%llu "
           "max_ns=%llu mean_ns=%.0Lf\n",
           scenario, extra, n,
           (unsigned long long)samples[n / 2],
           (unsigned long long)samples[(size_t)((long double)n * 0.99L)],
           (unsigned long long)samples[p999],
           (unsigned long long)samples[n - 1],
           sum / (long double)n);
    fflush(stdout);
}

/* ---- floor: mutex + condvar ping-pong --------------------------------- */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int turn; /* 0 = main's turn to send, 1 = peer's turn */
    uint64_t stamp;
    size_t hops, done;
    uint64_t* samples;
} CondPing;

static void* cond_peer(void* raw) {
    CondPing* p = (CondPing*)raw;
    pthread_mutex_lock(&p->mutex);
    while (p->done < p->hops) {
        while (p->turn != 1) pthread_cond_wait(&p->cond, &p->mutex);
        p->samples[p->done++] = now_ns() - p->stamp;
        p->stamp = now_ns();
        p->turn = 0;
        pthread_cond_signal(&p->cond);
    }
    pthread_mutex_unlock(&p->mutex);
    return NULL;
}

static void floor_condvar(size_t hops) {
    CondPing p = {0};
    pthread_mutex_init(&p.mutex, NULL);
    pthread_cond_init(&p.cond, NULL);
    p.hops = hops;
    p.samples = calloc(hops, sizeof(uint64_t));
    check(p.samples != NULL, "alloc");
    pthread_t peer;
    check(pthread_create(&peer, NULL, cond_peer, &p) == 0, "create");
    pthread_mutex_lock(&p.mutex);
    while (p.done < hops) {
        p.stamp = now_ns();
        p.turn = 1;
        pthread_cond_signal(&p.cond);
        while (p.turn != 0) pthread_cond_wait(&p.cond, &p.mutex);
    }
    pthread_mutex_unlock(&p.mutex);
    pthread_join(peer, NULL);
    report("floor_condvar", "kind=kernel_wake", p.samples, hops);
    free(p.samples);
}

/* ---- floor: spin ping-pong -------------------------------------------- */
typedef struct {
    _Alignas(64) _Atomic uint64_t to_peer;
    _Alignas(64) _Atomic uint64_t to_main;
    size_t hops;
    uint64_t* samples;
} SpinPing;

static void* spin_peer(void* raw) {
    SpinPing* p = (SpinPing*)raw;
    for (size_t i = 0; i < p->hops; ++i) {
        uint64_t stamp;
        while ((stamp = atomic_load_explicit(&p->to_peer, memory_order_acquire)) == 0)
            cpu_relax();
        atomic_store_explicit(&p->to_peer, 0, memory_order_relaxed);
        p->samples[i] = now_ns() - stamp;
        atomic_store_explicit(&p->to_main, 1, memory_order_release);
    }
    return NULL;
}

static void floor_spin(size_t hops) {
    SpinPing p;
    memset(&p, 0, sizeof(p));
    p.hops = hops;
    p.samples = calloc(hops, sizeof(uint64_t));
    check(p.samples != NULL, "alloc");
    pthread_t peer;
    check(pthread_create(&peer, NULL, spin_peer, &p) == 0, "create");
    for (size_t i = 0; i < hops; ++i) {
        uint64_t stamp = now_ns();
        atomic_store_explicit(&p.to_peer, stamp ? stamp : 1, memory_order_release);
        while (!atomic_load_explicit(&p.to_main, memory_order_acquire)) cpu_relax();
        atomic_store_explicit(&p.to_main, 0, memory_order_relaxed);
    }
    pthread_join(peer, NULL);
    report("floor_spin", "kind=cache_line", p.samples, hops);
    free(p.samples);
}

/* ---- scheduler ping-pong ---------------------------------------------- */
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef ref[2];
    size_t slot[2];
    size_t hops;
    _Atomic size_t done;
    _Atomic int finished;
    uint64_t* samples;
} SchedPing;

static void ping_handler(void* raw, size_t actor, uint64_t stamp) {
    SchedPing* p = (SchedPing*)raw;
    const uint64_t arrived = now_ns();
    /* Exactly one message is in flight, so hops never overlap. */
    size_t i = atomic_load_explicit(&p->done, memory_order_relaxed);
    p->samples[i] = arrived - stamp;
    atomic_store_explicit(&p->done, i + 1, memory_order_relaxed);
    if (i + 1 >= p->hops) {
        atomic_store_explicit(&p->finished, 1, memory_order_release);
        return;
    }
    const int to = actor == p->slot[0] ? 1 : 0;
    check(ores_native_actor_scheduler_try_send(p->scheduler, p->ref[to], now_ns())
              == ORES_NATIVE_SEND_ACCEPTED, "pingpong send");
}

static OresNativeActorScheduler* open_sched(
        size_t actors, size_t capacity, size_t workers,
        OresNativeActorTaskFn fn, void* ctx, uint32_t spin_ns,
        const uint32_t* cpus, size_t cpu_count) {
#ifdef ORESLANG_BENCH_HAS_TUNING
    const OresNativeActorSchedulerOptions options = {
        .actor_count = actors, .per_actor_capacity = capacity, .workers = workers,
        .on_message = fn, .ctx = ctx,
        .lock_protocol = g_lock_throughput ? ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT
                                           : ORES_NATIVE_SCHEDULER_LOCK_PRIORITY_INHERIT,
    };
    OresNativeActorScheduler* s = ores_native_actor_scheduler_open_with_options(&options);
#else
    OresNativeActorScheduler* s =
        ores_native_actor_scheduler_open(actors, capacity, workers, fn, ctx);
#endif
    check(s != NULL, "open");
#ifdef ORESLANG_BENCH_HAS_TUNING
    if (spin_ns)
        check(ores_native_actor_scheduler_configure_idle_spin(s, spin_ns),
              "configure_idle_spin");
    if (cpu_count && !ores_native_actor_scheduler_configure_cpu_affinity(
            s, cpus, cpu_count))
        fprintf(stderr, "note: CPU affinity unsupported on this host; unpinned\n");
#else
    (void)spin_ns; (void)cpus; (void)cpu_count;
#endif
    check(ores_native_actor_scheduler_prestart_carriers(s), "prestart");
    return s;
}

static void close_sched(OresNativeActorScheduler* s) {
    check(ores_native_actor_scheduler_drain(s), "drain");
    ores_native_actor_scheduler_retire(s);
    check(ores_native_actor_scheduler_try_destroy(s), "destroy");
}

static void sched_pingpong(size_t actors, size_t workers, size_t hops,
                           uint32_t spin_ns, const uint32_t* cpus, size_t ncpu) {
    SchedPing p;
    memset(&p, 0, sizeof(p));
    p.hops = hops;
    p.samples = calloc(hops, sizeof(uint64_t));
    check(p.samples != NULL, "alloc");
    p.slot[0] = 0;
    p.slot[1] = actors - 1;
    p.scheduler = open_sched(actors, 4, workers, ping_handler, &p, spin_ns, cpus, ncpu);
    /* Spawn every slot so a lane scan sees a realistic, fully populated VM. */
    for (size_t i = 0; i < actors; ++i) {
        OresNativeActorRef r = ores_native_actor_scheduler_spawn(p.scheduler, i);
        check(r.scheduler_domain != 0, "spawn");
        if (i == p.slot[0]) p.ref[0] = r;
        if (i == p.slot[1]) p.ref[1] = r;
    }
    check(ores_native_actor_scheduler_try_send(p.scheduler, p.ref[0], now_ns())
              == ORES_NATIVE_SEND_ACCEPTED, "first send");
    while (!atomic_load_explicit(&p.finished, memory_order_acquire)) {
        struct timespec pause = {0, 200000};
        nanosleep(&pause, NULL);
    }
    close_sched(p.scheduler);
    char extra[128];
    snprintf(extra, sizeof(extra), "actors=%zu workers=%zu spin_ns=%u pinned=%d lock=%s",
             actors, workers, spin_ns, ncpu > 0, g_lock_throughput ? "throughput" : "pi");
    report("sched_pingpong", extra, p.samples, hops);
    free(p.samples);
}

/* ---- scheduler fan-in throughput -------------------------------------- */
typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef* refs;
    size_t active, per_producer;
    _Atomic uint64_t turns;
} FanIn;

static void fanin_handler(void* raw, size_t actor, uint64_t payload) {
    (void)actor; (void)payload;
    FanIn* f = (FanIn*)raw;
    atomic_fetch_add_explicit(&f->turns, 1, memory_order_relaxed);
}

static void* fanin_producer(void* raw) {
    FanIn* f = (FanIn*)raw;
    size_t sent = 0, cursor = 0;
    while (sent < f->per_producer) {
        OresNativeSendStatus st = ores_native_actor_scheduler_try_send(
            f->scheduler, f->refs[cursor], sent);
        if (++cursor == f->active) cursor = 0;
        if (st == ORES_NATIVE_SEND_ACCEPTED) ++sent;
        else if (st == ORES_NATIVE_SEND_MAILBOX_FULL) cpu_relax();
        else check(0, "fanin send");
    }
    return NULL;
}

static void sched_fanin(size_t actors, size_t active, size_t workers,
                        size_t producers, size_t per_producer, uint32_t spin_ns,
                        const uint32_t* cpus, size_t ncpu) {
    FanIn f;
    memset(&f, 0, sizeof(f));
    f.active = active;
    f.per_producer = per_producer;
    f.refs = calloc(active, sizeof(*f.refs));
    check(f.refs != NULL, "alloc");
    f.scheduler = open_sched(actors, 256, workers, fanin_handler, &f, spin_ns, cpus, ncpu);
    /* Active actors are spread across the slot range. */
    for (size_t i = 0; i < actors; ++i) {
        OresNativeActorRef r = ores_native_actor_scheduler_spawn(f.scheduler, i);
        check(r.scheduler_domain != 0, "spawn");
        size_t stride = actors / active;
        if (i % stride == 0 && i / stride < active) f.refs[i / stride] = r;
    }
    pthread_t* tids = calloc(producers, sizeof(pthread_t));
    check(tids != NULL, "alloc");
    const uint64_t start = now_ns();
    for (size_t i = 0; i < producers; ++i)
        check(pthread_create(&tids[i], NULL, fanin_producer, &f) == 0, "create");
    for (size_t i = 0; i < producers; ++i) pthread_join(tids[i], NULL);
    check(ores_native_actor_scheduler_drain(f.scheduler), "drain");
    const uint64_t elapsed = now_ns() - start;
    const uint64_t turns = atomic_load(&f.turns);
    check(turns == producers * per_producer, "turn count");
    close_sched(f.scheduler);
    printf("scenario=sched_fanin actors=%zu active=%zu workers=%zu producers=%zu "
           "spin_ns=%u lock=%s turns=%llu elapsed_ns=%llu turns_per_sec=%.0f ns_per_turn=%.1f\n",
           actors, active, workers, producers, spin_ns,
           g_lock_throughput ? "throughput" : "pi",
           (unsigned long long)turns, (unsigned long long)elapsed,
           (double)turns * 1e9 / (double)elapsed, (double)elapsed / (double)turns);
    fflush(stdout);
    free(tids);
    free(f.refs);
}

#ifdef ORESLANG_BENCH_HAS_RT_DOMAIN
#include "oreslang/runtime/NativeRtDomain.h"
/* ---- per-core RT domain (NativeRtDomain.c) ---------------------------- */
typedef struct {
    uint32_t peer[2];
    size_t hops;
    _Atomic size_t done;
    _Atomic int finished;
    uint64_t* samples;
} RtPing;

static void rt_ping_handler(void* raw, OresRtSender* self, uint32_t actor, uint64_t stamp) {
    RtPing* p = raw;
    const uint64_t arrived = now_ns();
    size_t i = atomic_load_explicit(&p->done, memory_order_relaxed);
    p->samples[i] = arrived - stamp;
    atomic_store_explicit(&p->done, i + 1, memory_order_relaxed);
    if (i + 1 >= p->hops) {
        atomic_store_explicit(&p->finished, 1, memory_order_release);
        return;
    }
    check(ores_rt_send(self, actor == 0 ? 1 : 0, now_ns()) == ORES_RT_SEND_OK, "rt send");
}

/* Two actors on two different cores (cross-core hop) or one core. */
static void rt_pingpong(uint32_t cores, size_t hops, uint32_t spin_ns,
                        const uint32_t* cpus, size_t ncpu) {
    RtPing p;
    memset(&p, 0, sizeof(p));
    p.hops = hops;
    p.samples = calloc(hops, sizeof(uint64_t));
    check(p.samples != NULL, "alloc");
    uint32_t placement[2] = {0, cores - 1};
    OresRtDomainOptions o = {
        .core_count = cores, .cpus = ncpu >= cores ? cpus : NULL,
        .actor_count = 2, .actor_core = placement, .external_senders = 1,
        .ring_capacity = 64, .idle_spin_ns = spin_ns, .on_data = rt_ping_handler, .ctx = &p,
    };
    OresRtDomain* d = ores_rt_domain_open(&o);
    check(d != NULL, "rt open");
    check(ores_rt_domain_start(d), "rt start");
    check(ores_rt_send(ores_rt_domain_external_sender(d, 0), 0, now_ns()) == ORES_RT_SEND_OK,
          "rt kick");
    while (!atomic_load_explicit(&p.finished, memory_order_acquire)) {
        struct timespec pause = {0, 200000};
        nanosleep(&pause, NULL);
    }
    const int pinned = ores_rt_domain_affinity_observed(d);
    check(ores_rt_domain_stop(d), "rt stop");
    ores_rt_domain_destroy(d);
    char extra[128];
    snprintf(extra, sizeof(extra), "cores=%u spin_ns=%u pinned=%d %s",
             cores, spin_ns, pinned, cores > 1 ? "hop=cross_core" : "hop=same_core");
    report("rt_pingpong", extra, p.samples, hops);
    free(p.samples);
}

typedef struct {
    OresRtDomain* d;
    uint32_t id, actors;
    size_t each;
} RtProducer;
static _Atomic uint64_t g_rt_turns;
static void rt_count(void* ctx, OresRtSender* self, uint32_t actor, uint64_t payload) {
    (void)ctx; (void)self; (void)actor; (void)payload;
    atomic_fetch_add_explicit(&g_rt_turns, 1, memory_order_relaxed);
}
static void* rt_producer(void* raw) {
    RtProducer* a = raw;
    OresRtSender* s = ores_rt_domain_external_sender(a->d, a->id);
    uint32_t actor = a->id;
    for (size_t sent = 0; sent < a->each;) {
        if (ores_rt_send(s, actor, sent) == ORES_RT_SEND_OK) ++sent;
        else cpu_relax();
        if (++actor == a->actors) actor = 0;
    }
    return NULL;
}
static void rt_fanin(uint32_t cores, uint32_t producers, uint32_t actors, size_t each,
                     uint32_t spin_ns, const uint32_t* cpus, size_t ncpu) {
    uint32_t* placement = calloc(actors, sizeof(uint32_t));
    check(placement != NULL, "alloc");
    for (uint32_t a = 0; a < actors; ++a) placement[a] = a % cores;
    atomic_store(&g_rt_turns, 0);
    OresRtDomainOptions o = {
        .core_count = cores, .cpus = ncpu >= cores ? cpus : NULL,
        .actor_count = actors, .actor_core = placement, .external_senders = producers,
        .ring_capacity = 1024, .idle_spin_ns = spin_ns, .on_data = rt_count,
    };
    OresRtDomain* d = ores_rt_domain_open(&o);
    check(d && ores_rt_domain_start(d), "rt open/start");
    pthread_t t[ORES_RT_MAX_EXTERNAL];
    RtProducer args[ORES_RT_MAX_EXTERNAL];
    const uint64_t start = now_ns();
    for (uint32_t p = 0; p < producers; ++p) {
        args[p] = (RtProducer){d, p, actors, each};
        check(pthread_create(&t[p], NULL, rt_producer, &args[p]) == 0, "create");
    }
    for (uint32_t p = 0; p < producers; ++p) pthread_join(t[p], NULL);
    check(ores_rt_domain_stop(d), "rt stop");
    const uint64_t elapsed = now_ns() - start;
    const uint64_t turns = atomic_load(&g_rt_turns);
    check(turns == (uint64_t)producers * each, "rt turn count");
    ores_rt_domain_destroy(d);
    free(placement);
    printf("scenario=rt_fanin cores=%u producers=%u actors=%u spin_ns=%u turns=%llu "
           "elapsed_ns=%llu turns_per_sec=%.0f ns_per_turn=%.1f\n",
           cores, producers, actors, spin_ns, (unsigned long long)turns,
           (unsigned long long)elapsed, (double)turns * 1e9 / (double)elapsed,
           (double)elapsed / (double)turns);
    fflush(stdout);
}
#endif

int main(int argc, char** argv) {
    size_t hops = 200000;
    uint32_t spin_ns = 0;
    uint32_t cpus[128];
    size_t ncpu = 0;
    int smoke = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--smoke")) smoke = 1;
        else if (!strcmp(argv[i], "--hops") && i + 1 < argc)
            hops = (size_t)strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--spin-ns") && i + 1 < argc)
            spin_ns = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--lock") && i + 1 < argc) {
            const char* mode = argv[++i];
            if (!strcmp(mode, "throughput")) g_lock_throughput = 1;
            else if (strcmp(mode, "pi")) { fprintf(stderr, "--lock pi|throughput\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--cpus") && i + 1 < argc) {
            char* list = argv[++i];
            for (char* tok = strtok(list, ","); tok && ncpu < 128; tok = strtok(NULL, ","))
                cpus[ncpu++] = (uint32_t)strtoul(tok, NULL, 10);
        } else {
            fprintf(stderr, "usage: %s [--smoke] [--hops N] [--spin-ns N] [--cpus a,b] "
                            "[--lock pi|throughput]\n",
                    argv[0]);
            return 2;
        }
    }
    if (smoke) hops = 2000;
    const size_t fanin_each = smoke ? 2000 : 250000;

    floor_condvar(hops);
    floor_spin(hops);
    sched_pingpong(2, 2, hops, spin_ns, cpus, ncpu);
    sched_pingpong(1024, 2, hops, spin_ns, cpus, ncpu);
    sched_pingpong(1024, 8, hops, spin_ns, cpus, ncpu);
    sched_fanin(16, 16, 4, 4, fanin_each, spin_ns, cpus, ncpu);
    sched_fanin(1024, 4, 4, 4, fanin_each, spin_ns, cpus, ncpu);
    sched_fanin(1024, 64, 8, 4, fanin_each, spin_ns, cpus, ncpu);
#ifdef ORESLANG_BENCH_HAS_RT_DOMAIN
    rt_pingpong(1, hops, spin_ns, cpus, ncpu);
    rt_pingpong(2, hops, spin_ns, cpus, ncpu);
    rt_fanin(4, 4, 64, fanin_each, spin_ns, cpus, ncpu);
#endif
    return 0;
}
