#ifndef ATTO_ACTOR_ELASTIC_ADMISSION_V1_H
#define ATTO_ACTOR_ELASTIC_ADMISSION_V1_H
#include <pthread.h>
#include <stdint.h>

/* HOST-ONLY precursor: not currently wired to compiler-generated .atto guest turns. */
#define ATTO_ELASTIC_V1_WORKERS 64u
#define ATTO_ELASTIC_V1_POOL_SIZE UINT32_MAX

typedef enum {
    ATTO_ELASTIC_AFFINITY_NONE = 0,
    ATTO_ELASTIC_AFFINITY_STICKY = 1,
    ATTO_ELASTIC_AFFINITY_PINNED = 2,
    ATTO_ELASTIC_AFFINITY_DEDICATED = 3
} atto_elastic_affinity_v1;

typedef struct {
    uint64_t actor_incarnation;
    uint64_t pool_authority; /* HOST trusted opaque identity, never actor text */
    uint64_t ticket;
    uint64_t worker_generation;
    uint32_t lane;
} atto_elastic_lease_v1;

typedef struct {
    pthread_mutex_t mutex;
    uint64_t actor_incarnation;
    uint64_t pool_authority;
    uint64_t next_ticket;
    uint64_t home_generation;
    uint64_t live_generation[ATTO_ELASTIC_V1_WORKERS];
    uint64_t active_ticket[ATTO_ELASTIC_V1_WORKERS];
    uint64_t active_generation[ATTO_ELASTIC_V1_WORKERS];
    uint32_t max_parallelism;
    uint32_t actor_limit;
    uint32_t tenant_limit;
    uint32_t global_limit;
    uint32_t live_workers;
    uint32_t running;
    uint32_t peak;
    uint32_t home_lane;
    atto_elastic_affinity_v1 affinity;
    int has_home;
    int reserved_dedicated;
    int closed;
} atto_elastic_gate_v1;

/* return 1 on success, 0 if saturated/unavailable, -1 for invalid/stale. */
int atto_elastic_init_v1(atto_elastic_gate_v1 *gate, uint64_t actor_incarnation,
    uint64_t pool_authority, uint32_t max_parallelism, int serial,
    atto_elastic_affinity_v1 affinity, int reserved_dedicated,
    uint32_t actor_limit, uint32_t tenant_limit, uint32_t global_limit);
int atto_elastic_set_worker_v1(atto_elastic_gate_v1 *gate,
    uint32_t lane, uint64_t generation, int healthy);
uint32_t atto_elastic_capacity_v1(atto_elastic_gate_v1 *gate);
uint32_t atto_elastic_running_v1(atto_elastic_gate_v1 *gate);
int atto_elastic_try_enter_v1(atto_elastic_gate_v1 *gate,
    uint64_t actor_incarnation, uint64_t pool_authority,
    uint32_t lane, uint64_t worker_generation, atto_elastic_lease_v1 *out);
int atto_elastic_release_v1(atto_elastic_gate_v1 *gate, const atto_elastic_lease_v1 *lease);
void atto_elastic_stop_v1(atto_elastic_gate_v1 *gate);
/* fails closed while physical leases remain; must not destroy a live lock. */
int atto_elastic_destroy_v1(atto_elastic_gate_v1 *gate);
#endif
