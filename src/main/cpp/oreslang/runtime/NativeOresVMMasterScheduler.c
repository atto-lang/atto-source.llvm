#include "oreslang/runtime/NativeOresVMMasterScheduler.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

struct OresNativeVMMasterScheduler {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    OresNativeActorScheduler* pools[3]; /* borrowed, detach before pool free */
    unsigned pending;                    /* one coalesced bit per pool */
    uint64_t delivered;
    int retired;
};
static int valid_kind(OresNativeVMPoolKind kind) {
    return kind >= ORES_VM_POOL_SHARED && kind <= ORES_VM_POOL_UNTRUSTED;
}
static void must_lock(OresNativeVMMasterScheduler* s) {
    if (pthread_mutex_lock(&s->lock) != 0) abort();
}
static void must_unlock(OresNativeVMMasterScheduler* s) {
    if (pthread_mutex_unlock(&s->lock) != 0) abort();
}
static void* master_control_loop(void* arg) {
    OresNativeVMMasterScheduler* m = (OresNativeVMMasterScheduler*)arg;
    must_lock(m);
    for (;;) {
        while (!m->retired && m->pending == 0) {
            if (pthread_cond_wait(&m->ready, &m->lock) != 0) abort();
        }
        if (m->retired) { must_unlock(m); return NULL; }
        unsigned batch = m->pending;
        m->pending = 0;
        /* Keep the master monitor across each short pool notification:
         * detach() then fences all in-flight dereferences without holding
         * any worker/actor callback or invoking guest code. Lock order is
         * master -> pool; pool code never calls master while holding pool
         * lock, so this does not introduce an inversion. */
        for (unsigned i = 0; i < 3; i++) {
            if ((batch & (1u << i)) == 0 || m->pools[i] == NULL) continue;
            (void)ores_native_actor_scheduler_signal_control(m->pools[i]);
            if (m->delivered != UINT64_MAX) ++m->delivered;
        }
    }
}
OresNativeVMMasterScheduler* ores_native_vm_master_open(void) {
    OresNativeVMMasterScheduler* m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->lock, NULL) != 0) {
        free(m); return NULL;
    }
    if (pthread_cond_init(&m->ready, NULL) != 0) {
        pthread_mutex_destroy(&m->lock);
        free(m);
        return NULL;
    }
    if (pthread_create(&m->thread, NULL, master_control_loop, m) != 0) {
        pthread_cond_destroy(&m->ready);
        pthread_mutex_destroy(&m->lock);
        free(m);
        return NULL;
    }
    return m;
}
int ores_native_vm_master_attach(
    OresNativeVMMasterScheduler* m, OresNativeVMPoolKind kind,
    OresNativeActorScheduler* pool) {
    if (!m || !pool || !valid_kind(kind)) return 0;
    must_lock(m);
    int ok = !m->retired && !m->pools[kind];
    /* Each pool has exactly one control authority in this master. */
    for (unsigned i = 0; i < 3 && ok; i++) {
        if (m->pools[i] == pool) ok = 0;
    }
    if (ok) m->pools[kind] = pool;
    must_unlock(m);
    return ok;
}
int ores_native_vm_master_detach(
    OresNativeVMMasterScheduler* m, OresNativeVMPoolKind kind,
    OresNativeActorScheduler* pool) {
    if (!m || !pool || !valid_kind(kind)) return 0;
    must_lock(m);
    int ok = m->pools[kind] == pool;
    if (ok) {
        m->pools[kind] = NULL;
        m->pending &= ~(1u << (unsigned)kind);
    }
    must_unlock(m);
    return ok;
}
int ores_native_vm_master_notify(
    OresNativeVMMasterScheduler* m, OresNativeVMPoolKind kind) {
    if (!m || !valid_kind(kind)) return 0;
    must_lock(m);
    int ok = !m->retired && m->pools[kind] != NULL;
    if (ok) {
        m->pending |= (1u << (unsigned)kind);
        if (pthread_cond_signal(&m->ready) != 0) abort();
    }
    must_unlock(m);
    return ok;
}
uint64_t ores_native_vm_master_delivered_wakes(OresNativeVMMasterScheduler* m) {
    if (!m) return 0;
    must_lock(m);
    uint64_t result = m->delivered;
    must_unlock(m);
    return result;
}
size_t ores_native_vm_master_started_control_threads(OresNativeVMMasterScheduler* m) {
    /* A valid instance owns exactly one started master thread until destroy. */
    return m ? 1u : 0u;
}
void ores_native_vm_master_retire(OresNativeVMMasterScheduler* m) {
    if (!m) return;
    must_lock(m);
    m->retired = 1;
    m->pending = 0;
    if (pthread_cond_broadcast(&m->ready) != 0) abort();
    must_unlock(m);
}
int ores_native_vm_master_try_destroy(OresNativeVMMasterScheduler* m) {
    if (!m || pthread_equal(pthread_self(), m->thread)) return 0;
    must_lock(m);
    int ready = m->retired;
    for (unsigned i = 0; i < 3; i++) {
        if (m->pools[i]) ready = 0;
    }
    must_unlock(m);
    if (!ready) return 0;
    /* The host fences other API users before this final free. */
    if (pthread_join(m->thread, NULL) != 0) abort();
    if (pthread_cond_destroy(&m->ready) != 0 ||
        pthread_mutex_destroy(&m->lock) != 0) abort();
    free(m);
    return 1;
}
