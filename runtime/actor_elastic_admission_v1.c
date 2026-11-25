#include "actor_elastic_admission_v1.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

int atto_elastic_init_v1(atto_elastic_gate_v1 *gate, uint64_t actor_incarnation,
    uint64_t pool_authority, uint32_t max_parallelism, int serial,
    atto_elastic_affinity_v1 affinity, int reserved_dedicated,
    uint32_t actor_limit, uint32_t tenant_limit, uint32_t global_limit) {
    if (!gate || !actor_incarnation || !pool_authority ||
        max_parallelism == 0 || !actor_limit || !tenant_limit || !global_limit ||
        (serial && max_parallelism != 1) ||
        (affinity < ATTO_ELASTIC_AFFINITY_NONE ||
         affinity > ATTO_ELASTIC_AFFINITY_DEDICATED) ||
        ((affinity == ATTO_ELASTIC_AFFINITY_PINNED ||
          affinity == ATTO_ELASTIC_AFFINITY_DEDICATED) && max_parallelism != 1)) return -1;
    memset(gate, 0, sizeof(*gate));
    if (pthread_mutex_init(&gate->mutex, NULL)) return -1;
    gate->actor_incarnation = actor_incarnation;
    gate->pool_authority = pool_authority;
    gate->max_parallelism = max_parallelism;
    gate->affinity = affinity;
    gate->reserved_dedicated = reserved_dedicated;
    gate->actor_limit = actor_limit;
    gate->tenant_limit = tenant_limit;
    gate->global_limit = global_limit;
    return 1;
}

int atto_elastic_set_worker_v1(atto_elastic_gate_v1 *gate, uint32_t lane,
    uint64_t generation, int healthy) {
    if (!gate || lane >= ATTO_ELASTIC_V1_WORKERS || (healthy && !generation)) return -1;
    pthread_mutex_lock(&gate->mutex);
    if (gate->closed) { pthread_mutex_unlock(&gate->mutex); return -1; }
    if (gate->live_generation[lane] == 0 && healthy) gate->live_workers++;
    else if (gate->live_generation[lane] && !healthy) gate->live_workers--;
    gate->live_generation[lane] = healthy ? generation : 0;
    if (gate->running == 0 && gate->has_home &&
        gate->live_generation[gate->home_lane] != gate->home_generation)
        gate->has_home = 0;
    pthread_mutex_unlock(&gate->mutex);
    return 1;
}

static uint32_t capacity_locked(const atto_elastic_gate_v1 *gate) {
    uint32_t limit = gate->max_parallelism == ATTO_ELASTIC_V1_POOL_SIZE
        ? gate->live_workers : gate->max_parallelism;
    limit = min_u32(limit, gate->live_workers);
    limit = min_u32(limit, gate->actor_limit);
    limit = min_u32(limit, gate->tenant_limit);
    return min_u32(limit, gate->global_limit);
}
uint32_t atto_elastic_capacity_v1(atto_elastic_gate_v1 *gate) {
    if (!gate) return 0;
    pthread_mutex_lock(&gate->mutex);
    uint32_t cap = capacity_locked(gate);
    pthread_mutex_unlock(&gate->mutex);
    return cap;
}
uint32_t atto_elastic_running_v1(atto_elastic_gate_v1 *gate) {
    if (!gate) return 0;
    pthread_mutex_lock(&gate->mutex);
    uint32_t running = gate->running;
    pthread_mutex_unlock(&gate->mutex);
    return running;
}

int atto_elastic_try_enter_v1(atto_elastic_gate_v1 *gate,
    uint64_t actor_incarnation, uint64_t pool_authority,
    uint32_t lane, uint64_t worker_generation, atto_elastic_lease_v1 *out) {
    if (!gate || !out || lane >= ATTO_ELASTIC_V1_WORKERS || !worker_generation ||
        actor_incarnation != gate->actor_incarnation || pool_authority != gate->pool_authority)
        return -1;
    pthread_mutex_lock(&gate->mutex);
    if (gate->closed || gate->live_generation[lane] != worker_generation ||
        gate->running >= capacity_locked(gate) || gate->active_ticket[lane] ||
        (gate->affinity == ATTO_ELASTIC_AFFINITY_DEDICATED && !gate->reserved_dedicated)) {
        pthread_mutex_unlock(&gate->mutex); return 0;
    }
    if ((gate->affinity == ATTO_ELASTIC_AFFINITY_PINNED ||
         gate->affinity == ATTO_ELASTIC_AFFINITY_DEDICATED) && gate->has_home &&
        (gate->home_lane != lane || gate->home_generation != worker_generation)) {
        if (gate->running ||
            gate->live_generation[gate->home_lane] == gate->home_generation) {
            pthread_mutex_unlock(&gate->mutex); return 0;
        }
    }
    if (gate->next_ticket == UINT64_MAX) { pthread_mutex_unlock(&gate->mutex); return -1; }
    const uint64_t ticket = ++gate->next_ticket;
    gate->active_ticket[lane] = ticket;
    gate->active_generation[lane] = worker_generation;
    gate->running++;
    if (gate->running > gate->peak) gate->peak = gate->running;
    if (!gate->has_home || gate->affinity >= ATTO_ELASTIC_AFFINITY_PINNED) {
        gate->home_lane = lane;
        gate->home_generation = worker_generation;
        gate->has_home = 1;
    }
    *out = (atto_elastic_lease_v1){actor_incarnation, pool_authority,
        ticket, worker_generation, lane};
    pthread_mutex_unlock(&gate->mutex);
    return 1;
}

int atto_elastic_release_v1(atto_elastic_gate_v1 *gate,
    const atto_elastic_lease_v1 *lease) {
    if (!gate || !lease || lease->lane >= ATTO_ELASTIC_V1_WORKERS ||
        lease->actor_incarnation != gate->actor_incarnation ||
        lease->pool_authority != gate->pool_authority || lease->ticket == 0) return -1;
    pthread_mutex_lock(&gate->mutex);
    const uint32_t lane = lease->lane;
    if (gate->active_ticket[lane] != lease->ticket ||
        gate->active_generation[lane] != lease->worker_generation || !gate->running) {
        pthread_mutex_unlock(&gate->mutex); return -1;
    }
    gate->active_ticket[lane] = 0;
    gate->active_generation[lane] = 0;
    gate->running--;
    if (gate->running == 0 && gate->has_home &&
        gate->live_generation[gate->home_lane] != gate->home_generation)
        gate->has_home = 0;
    pthread_mutex_unlock(&gate->mutex);
    return 1;
}
void atto_elastic_stop_v1(atto_elastic_gate_v1 *gate) {
    if (!gate) return;
    pthread_mutex_lock(&gate->mutex);
    gate->closed = 1;
    pthread_mutex_unlock(&gate->mutex);
}
int atto_elastic_destroy_v1(atto_elastic_gate_v1 *gate) {
    if (!gate) return -1;
    pthread_mutex_lock(&gate->mutex);
    if (gate->running || !gate->closed) { pthread_mutex_unlock(&gate->mutex); return -1; }
    pthread_mutex_unlock(&gate->mutex);
    return pthread_mutex_destroy(&gate->mutex) == 0 ? 1 : -1;
}
