#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dedicated per-core real-time scheduler domain (docs/HARD_REALTIME.md,
 * "Scheduler work"). Separate from NativeActorScheduler's general domain:
 *
 *  - one carrier thread per core, optionally pinned (Linux) and SCHED_FIFO;
 *  - every actor is statically placed on one core at open, so an actor only
 *    ever runs on that core's single carrier: exclusivity by construction,
 *    no scheduler lock anywhere;
 *  - messages travel through a fixed matrix of single-producer/single-consumer
 *    rings, one per (sender, target core) and lane (CONTROL before DATA);
 *    senders are the cores themselves plus a fixed set of external producers;
 *  - all memory is allocated and touched at open; send/dispatch never
 *    allocate, lock or make a syscall (except waking a parked core);
 *  - an idle carrier spins for a bounded budget, then parks.
 *
 * Status: substrate only. It provides the bounded, lock-free dispatch path a
 * hard-RT admission needs; it does not itself prove schedulability, WCET,
 * IRQ isolation or memory residency (see LinuxRealtimeEnvironment.h).
 */

typedef struct OresRtDomain OresRtDomain;
/* A send endpoint owned by exactly one thread for its whole life:
 * core senders belong to their carrier; external senders to the first thread
 * that uses them. Use from any other thread returns ORES_RT_SEND_INVALID. */
typedef struct OresRtSender OresRtSender;

/* Runs on the actor's core. `self` is that core's sender: use it to send
 * from inside a handler. Handlers must not block or spin on a full ring. */
typedef void (*OresRtHandlerFn)(void* ctx, OresRtSender* self,
                                uint32_t actor, uint64_t payload);

enum {
    ORES_RT_MAX_CORES = 64,
    ORES_RT_MAX_EXTERNAL = 64,
    ORES_RT_MAX_ACTORS = 65536,
    ORES_RT_MAX_RING = 65536,
    ORES_RT_MAX_IDLE_SPIN_NS = 1000000
};

typedef struct {
    uint32_t core_count;          /* 1..ORES_RT_MAX_CORES */
    const uint32_t* cpus;         /* optional: core i pinned to cpus[i] (Linux) */
    int sched_fifo_priority;      /* 0 = inherit policy; else SCHED_FIFO prio */
    uint32_t actor_count;         /* 1..ORES_RT_MAX_ACTORS */
    const uint32_t* actor_core;   /* required: actor -> core index */
    uint32_t external_senders;    /* 0..ORES_RT_MAX_EXTERNAL */
    uint32_t ring_capacity;       /* power of two, 2..ORES_RT_MAX_RING */
    uint32_t idle_spin_ns;        /* 0 = park immediately */
    int measure_turns;            /* record max handler time per core */
    uint64_t turn_budget_ns;      /* 0 = none; else count turns longer than this
                                     (implies measure_turns). Diagnostic only:
                                     an overrun is detected, not prevented. */
    OresRtHandlerFn on_data;      /* required */
    OresRtHandlerFn on_control;   /* optional; CONTROL sends fail without it */
    void* ctx;
} OresRtDomainOptions;

typedef enum {
    ORES_RT_SEND_OK = 0,
    ORES_RT_SEND_FULL = 1,     /* ring to that core is full; caller keeps item */
    ORES_RT_SEND_INVALID = 2,  /* bad actor, wrong thread, or no handler */
    ORES_RT_SEND_CLOSED = 3    /* domain not started or stopping */
} OresRtSendStatus;

typedef struct {
    uint64_t processed;          /* DATA + CONTROL turns */
    uint64_t control_processed;
    uint64_t parks;              /* times the carrier slept in the kernel */
    uint64_t max_turn_ns;        /* 0 unless measure_turns / turn_budget_ns */
    uint64_t budget_overruns;    /* turns longer than turn_budget_ns */
} OresRtCoreStats;

/* Validates and preallocates everything (and touches it). NULL on bad
 * options. No threads exist until start. */
OresRtDomain* ores_rt_domain_open(const OresRtDomainOptions* options);
/* Creates every carrier (pinned / SCHED_FIFO as configured) and returns only
 * after all of them run. 0 if any carrier cannot be created with the
 * requested policy; then nothing was started. */
int ores_rt_domain_start(OresRtDomain* domain);
/* 1 iff cpus were given and every carrier read back an OS affinity of
 * exactly its configured CPU and was running on it. */
int ores_rt_domain_affinity_observed(OresRtDomain* domain);

OresRtSender* ores_rt_domain_external_sender(OresRtDomain* domain, uint32_t index);
OresRtSendStatus ores_rt_send(OresRtSender* sender, uint32_t actor, uint64_t payload);
OresRtSendStatus ores_rt_send_control(OresRtSender* sender, uint32_t actor,
                                      uint64_t payload);
/* Messages this sender got ORES_RT_SEND_OK for. */
uint64_t ores_rt_sender_accepted(const OresRtSender* sender);
/* Hand an external sender to another thread. Only its current owner may
 * call this; the next thread to send becomes the owner, ordered after
 * everything the previous owner published. Returns 0 if the caller is not
 * the owner or the sender belongs to a core. */
int ores_rt_sender_release(OresRtSender* sender);

/* Waits until every accepted message (including ones sent by handlers while
 * draining) has run, then stops and joins all carriers. The caller must have
 * stopped using external senders. Never call from a handler. */
int ores_rt_domain_stop(OresRtDomain* domain);
/* As stop, but gives up after timeout_ns if the domain does not become
 * quiescent (e.g. a handler is stuck). Returns 1 stopped, 0 timed out (the
 * domain keeps running and stop may be retried), -1 invalid call. */
int ores_rt_domain_stop_timed(OresRtDomain* domain, uint64_t timeout_ns);
/* After stop: messages that were accepted but never ran. Nonzero only if
 * external senders kept sending during stop (a caller contract violation),
 * so the loss is reported instead of silent. */
uint64_t ores_rt_domain_undelivered(OresRtDomain* domain);
int ores_rt_domain_core_stats(OresRtDomain* domain, uint32_t core,
                              OresRtCoreStats* out);
/* Only after stop (or if never started). */
void ores_rt_domain_destroy(OresRtDomain* domain);

#ifdef __cplusplus
}
#endif
