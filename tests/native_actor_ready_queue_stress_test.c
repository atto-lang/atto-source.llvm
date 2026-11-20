/* O(1) ready-queue scheduler stress + bootstrap tuning API contract.
 *
 * Built with ORESLANG_NATIVE_SCHED_VALIDATE so every queue mutation is
 * cross-checked against an O(N) scan of lane state, and (in a second target)
 * under ThreadSanitizer.
 *
 * Stress: producers flood random actors while another thread publishes
 * CONTROL and seals actors mid-flight, with idle spin both off and on.
 * Checks, per actor:
 *   - at most one callback at a time (exclusivity),
 *   - each producer's DATA arrives in send order (per-sender FIFO),
 *   - accepted == processed + abandoned, matching the producers' own count of
 *     accepted sends and the handler's own count of processed turns,
 *   - every accepted CONTROL ran exactly once.
 */
#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* cpu_set_t, sched_getaffinity */
#endif
#endif
#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeActorScheduler.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sched.h>
#endif

#define ACTORS 64
#define CAPACITY 16
#define WORKERS 6
#define PRODUCERS 4
#define SENDS_PER_PRODUCER 20000
#define SEALED_ACTORS 8

static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "ready-queue stress failed: %s\n", what);
        abort();
    }
}

typedef struct {
    OresNativeActorScheduler* scheduler;
    OresNativeActorRef refs[ACTORS];
    atomic_int inside[ACTORS];
    /* Highest sequence seen per (actor, producer); payload = producer<<32|seq. */
    _Atomic uint64_t last_seq[ACTORS][PRODUCERS];
    _Atomic uint64_t processed[ACTORS];
    _Atomic uint64_t accepted[ACTORS];
    _Atomic uint64_t controls_run, controls_accepted;
    atomic_int errors;
    atomic_int producers_done;
} Stress;

static void enter(Stress* st, size_t actor) {
    if (atomic_fetch_add(&st->inside[actor], 1) != 0) atomic_fetch_add(&st->errors, 1);
}
static void leave(Stress* st, size_t actor) {
    atomic_fetch_sub(&st->inside[actor], 1);
}
static void on_message(void* ctx, size_t actor, uint64_t payload) {
    Stress* st = (Stress*)ctx;
    enter(st, actor);
    const size_t producer = (size_t)(payload >> 32);
    const uint64_t seq = payload & 0xffffffffu;
    if (producer >= PRODUCERS) {
        atomic_fetch_add(&st->errors, 1);
    } else {
        const uint64_t prev = atomic_load(&st->last_seq[actor][producer]);
        if (seq <= prev) atomic_fetch_add(&st->errors, 1); /* FIFO violated */
        atomic_store(&st->last_seq[actor][producer], seq);
    }
    atomic_fetch_add(&st->processed[actor], 1);
    leave(st, actor);
}
static void on_control(void* ctx, size_t actor, uint64_t payload) {
    Stress* st = (Stress*)ctx;
    enter(st, actor);
    if (payload != 7) atomic_fetch_add(&st->errors, 1);
    atomic_fetch_add(&st->controls_run, 1);
    leave(st, actor);
}

typedef struct { Stress* st; size_t id; } ProducerArg;

static void* producer(void* raw) {
    ProducerArg* arg = (ProducerArg*)raw;
    Stress* st = arg->st;
    uint64_t seq = 1;
    uint32_t rng = 0x9e3779b9u * (uint32_t)(arg->id + 1);
    for (size_t sent = 0; sent < SENDS_PER_PRODUCER; ++sent) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        const size_t actor = rng % ACTORS;
        const uint64_t payload = ((uint64_t)arg->id << 32) | seq;
        for (;;) {
            OresNativeSendStatus status =
                ores_native_actor_scheduler_try_send(st->scheduler, st->refs[actor], payload);
            if (status == ORES_NATIVE_SEND_ACCEPTED) {
                atomic_fetch_add(&st->accepted[actor], 1);
                ++seq;
                break;
            }
            if (status == ORES_NATIVE_SEND_TERMINATED) break; /* sealed: drop */
            check(status == ORES_NATIVE_SEND_MAILBOX_FULL, "unexpected send status");
            sched_yield();
        }
    }
    atomic_fetch_add(&st->producers_done, 1);
    return NULL;
}

/* Publishes CONTROL to live actors and seals a few actors mid-flight. */
static void* disruptor(void* raw) {
    Stress* st = (Stress*)raw;
    size_t round = 0;
    while (atomic_load(&st->producers_done) < PRODUCERS) {
        const size_t actor = SEALED_ACTORS + (round % (ACTORS - SEALED_ACTORS));
        if (ores_native_actor_scheduler_publish_control(st->scheduler, st->refs[actor], 7))
            atomic_fetch_add(&st->controls_accepted, 1);
        if (round < SEALED_ACTORS && round % 2 == 0)
            check(ores_native_actor_scheduler_stop_sealed(st->scheduler, st->refs[round]),
                  "stop_sealed");
        else if (round < SEALED_ACTORS)
            check(ores_native_actor_scheduler_stop(st->scheduler, st->refs[round]), "stop");
        ++round;
        sched_yield();
    }
    return NULL;
}

static void run_stress(uint32_t spin_ns, OresNativeSchedulerLockProtocol protocol) {
    Stress* st = calloc(1, sizeof(*st));
    check(st != NULL, "alloc");
    const OresNativeActorSchedulerOptions options = {
        .actor_count = ACTORS, .per_actor_capacity = CAPACITY, .workers = WORKERS,
        .on_message = on_message, .on_control = on_control, .ctx = st,
        .lock_protocol = protocol,
    };
    st->scheduler = ores_native_actor_scheduler_open_with_options(&options);
    check(st->scheduler != NULL, "open");
    check(ores_native_actor_scheduler_configure_idle_spin(st->scheduler, spin_ns), "spin");
    for (size_t i = 0; i < ACTORS; ++i) {
        st->refs[i] = ores_native_actor_scheduler_spawn(st->scheduler, i);
        check(st->refs[i].scheduler_domain != 0, "spawn");
    }
    pthread_t producers[PRODUCERS], dis;
    ProducerArg args[PRODUCERS];
    for (size_t p = 0; p < PRODUCERS; ++p) {
        args[p] = (ProducerArg){st, p};
        check(pthread_create(&producers[p], NULL, producer, &args[p]) == 0, "create");
    }
    check(pthread_create(&dis, NULL, disruptor, st) == 0, "create");
    for (size_t p = 0; p < PRODUCERS; ++p) pthread_join(producers[p], NULL);
    pthread_join(dis, NULL);

    check(ores_native_actor_scheduler_drain(st->scheduler), "drain");
    ores_native_actor_scheduler_retire(st->scheduler);
    check(ores_native_actor_scheduler_drain(st->scheduler), "drain after retire");

    uint64_t total_processed = 0;
    for (size_t i = 0; i < ACTORS; ++i) {
        OresNativeActorTerminalState term;
        check(ores_native_actor_scheduler_wait_done(st->scheduler, st->refs[i]), "wait_done");
        check(ores_native_actor_scheduler_terminal_state(st->scheduler, st->refs[i], &term),
              "terminal state");
        check(term.accepted_user_messages == atomic_load(&st->accepted[i]),
              "accepted matches producer count");
        check(term.processed_user_messages == atomic_load(&st->processed[i]),
              "processed matches handler count");
        check(term.processed_user_messages + term.abandoned_user_messages
                  == term.accepted_user_messages, "processed + abandoned == accepted");
        if (i >= SEALED_ACTORS)
            check(term.abandoned_user_messages == 0, "unsealed actor abandoned mail");
        total_processed += term.processed_user_messages;
    }
    check(atomic_load(&st->controls_run) == atomic_load(&st->controls_accepted),
          "every accepted CONTROL ran exactly once");
    check(ores_native_actor_scheduler_control_completed(st->scheduler)
              == atomic_load(&st->controls_accepted), "control_completed");
    check(atomic_load(&st->errors) == 0, "exclusivity/FIFO errors");
    check(ores_native_actor_scheduler_try_destroy(st->scheduler), "destroy");
    printf("lock=%s spin_ns=%u processed=%llu controls=%llu: OK\n",
           protocol == ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT ? "throughput" : "pi", spin_ns,
           (unsigned long long)total_processed,
           (unsigned long long)atomic_load(&st->controls_accepted));
    free(st);
}

static void noop(void* ctx, size_t actor, uint64_t payload) {
    (void)ctx; (void)actor; (void)payload;
}

static void tuning_contract(void) {
    OresNativeActorScheduler* s = ores_native_actor_scheduler_open(2, 4, 2, noop, NULL);
    check(s != NULL, "open");
    check(!ores_native_actor_scheduler_configure_idle_spin(s, 1000001), "spin cap");
    check(ores_native_actor_scheduler_configure_idle_spin(s, 5000), "spin pristine");
    check(!ores_native_actor_scheduler_configure_cpu_affinity(s, NULL, 1), "null cpus");
    check(!ores_native_actor_scheduler_cpu_affinity_observed(s), "observed before config");
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    check(sched_getaffinity(0, sizeof(allowed), &allowed) == 0, "getaffinity");
    uint32_t cpus[2];
    size_t n = 0;
    for (uint32_t c = 0; c < CPU_SETSIZE && n < 2; ++c)
        if (CPU_ISSET(c, &allowed)) cpus[n++] = c;
    uint32_t outside = CPU_SETSIZE;
    for (uint32_t c = 0; c < CPU_SETSIZE; ++c)
        if (!CPU_ISSET(c, &allowed)) { outside = c; break; }
    if (outside < CPU_SETSIZE)
        check(!ores_native_actor_scheduler_configure_cpu_affinity(s, &outside, 1),
              "CPU outside the process mask must be rejected");
    check(ores_native_actor_scheduler_configure_cpu_affinity(s, cpus, n), "affinity");
    check(ores_native_actor_scheduler_prestart_carriers(s), "prestart");
    check(ores_native_actor_scheduler_cpu_affinity_observed(s),
          "every carrier must observe its configured CPU");
#else
    const uint32_t cpu = 0;
    check(!ores_native_actor_scheduler_configure_cpu_affinity(s, &cpu, 1),
          "affinity must fail closed without OS support");
    check(ores_native_actor_scheduler_prestart_carriers(s), "prestart");
    check(!ores_native_actor_scheduler_cpu_affinity_observed(s), "no affinity observed");
#endif
    /* Bootstrap-only once carriers exist. */
    check(!ores_native_actor_scheduler_configure_idle_spin(s, 0), "spin after start");
    check(!ores_native_actor_scheduler_configure_cpu_affinity(s, (const uint32_t[]){0}, 1),
          "affinity after start");
    OresNativeActorRef a = ores_native_actor_scheduler_spawn(s, 0);
    check(ores_native_actor_scheduler_send(s, a, 1), "send");
    check(ores_native_actor_scheduler_drain(s), "drain");
    ores_native_actor_scheduler_retire(s);
    check(ores_native_actor_scheduler_try_destroy(s), "destroy");
    puts("tuning contract: OK");
}

static void lock_protocol_contract(void) {
    OresNativeActorSchedulerOptions options = {
        .actor_count = 1, .per_actor_capacity = 1, .workers = 1, .on_message = noop,
        .lock_protocol = (OresNativeSchedulerLockProtocol)7,
    };
    check(ores_native_actor_scheduler_open_with_options(NULL) == NULL, "NULL options");
    check(ores_native_actor_scheduler_open_with_options(&options) == NULL,
          "unknown lock protocol must be rejected");
    options.lock_protocol = ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT;
    OresNativeActorScheduler* s = ores_native_actor_scheduler_open_with_options(&options);
    check(s != NULL, "open throughput");
    /* No PI on the scheduler lock: hard-RT admission must see that. */
    check(!ores_native_actor_scheduler_priority_inheritance_enabled(s),
          "throughput lock must not report priority inheritance");
    ores_native_actor_scheduler_retire(s);
    check(ores_native_actor_scheduler_try_destroy(s), "destroy");
    puts("lock protocol contract: OK");
}

int main(void) {
    tuning_contract();
    lock_protocol_contract();
    run_stress(0, ORES_NATIVE_SCHEDULER_LOCK_PRIORITY_INHERIT);
    run_stress(20000, ORES_NATIVE_SCHEDULER_LOCK_PRIORITY_INHERIT);
    run_stress(0, ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT);
    run_stress(20000, ORES_NATIVE_SCHEDULER_LOCK_THROUGHPUT);
    puts("native actor O(1) ready queue stress: OK");
    return 0;
}
