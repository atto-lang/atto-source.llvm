#include "oreslang/runtime/NativeExecutionAuthority.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>

enum { CARRIERS = 12 };
typedef struct {
    OresNativeExecutionAuthority* authority;
    _Atomic int* entered;
    _Atomic int* release;
    _Atomic int* winner_count;
    uint64_t carrier;
} ClaimArg;

static void* contender(void* raw) {
    ClaimArg* a = (ClaimArg*)raw;
    OresExecutionClaim claim = {0};
    int won = ores_execution_enter(a->authority, 1, a->carrier, 0, &claim);
    if (won) atomic_fetch_add(a->winner_count, 1);
    atomic_fetch_add(a->entered, 1);
    if (won) {
        while (!atomic_load(a->release)) sched_yield();
        assert(ores_execution_owns(a->authority, claim));
        assert(ores_execution_leave(a->authority, claim, 0));
        assert(!ores_execution_leave(a->authority, claim, 0));
    }
    return NULL;
}

static void concurrent_carriers(void) {
    OresNativeExecutionAuthority authority;
    assert(ores_execution_init(&authority));
    _Atomic int entered = 0, release = 0, winners = 0;
    ClaimArg args[CARRIERS];
    pthread_t threads[CARRIERS];
    for (int i = 0; i < CARRIERS; ++i) {
        args[i] = (ClaimArg){&authority, &entered, &release, &winners, (uint64_t)(i + 1)};
        assert(pthread_create(&threads[i], NULL, contender, &args[i]) == 0);
    }
    while (atomic_load(&entered) != CARRIERS) sched_yield();
    assert(atomic_load(&winners) == 1);
    assert(ores_execution_phase(&authority) == ORES_EXEC_RUNNING);
    OresExecutionClaim forbidden = {0};
    assert(!ores_execution_enter(&authority, 1, 99, 0, &forbidden));
    atomic_store(&release, 1);
    for (int i = 0; i < CARRIERS; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(ores_execution_phase(&authority) == ORES_EXEC_READY);
    assert(ores_execution_destroy(&authority));
}
static void suspend_stop_restart(void) {
    OresNativeExecutionAuthority a;
    assert(ores_execution_init(&a));
    OresExecutionClaim x={0}, y={0};
    assert(ores_execution_enter(&a, 1, 7, 0, &x));
    assert(!ores_execution_enter(&a, 1, 8, 0, &y));
    assert(ores_execution_leave(&a, x, 1));
    assert(ores_execution_phase(&a) == ORES_EXEC_SUSPENDED);
    assert(ores_execution_enter(&a, 1, 8, 0, &y));
    assert(!ores_execution_leave(&a, x, 0));
    assert(!ores_execution_retired(&a, 1));
    assert(ores_execution_stop(&a, 1));
    assert(!ores_execution_retired(&a, 1));
    assert(!ores_execution_enter(&a, 1, 9, 0, &x));
    assert(ores_execution_leave(&a, y, 0));
    assert(ores_execution_retired(&a, 1));
    assert(!ores_execution_retired(&a, 1));
    assert(ores_execution_restart(&a) == 2);
    assert(!ores_execution_enter(&a, 1, 8, 0, &x));
    assert(ores_execution_enter(&a, 2, 9, 0, &x));
    assert(!ores_execution_leave(&a, y, 0));
    assert(ores_execution_leave(&a, x, 0));
    assert(ores_execution_stop(&a, 2));
    assert(ores_execution_retired(&a, 2));
    assert(ores_execution_destroy(&a));
}
static void preadmitted_drain_and_exhaustion(void) {
    OresNativeExecutionAuthority a;
    assert(ores_execution_init(&a));
    assert(ores_execution_stop(&a, 1));
    OresExecutionClaim x={0};
    assert(!ores_execution_enter(&a, 1, 2, 0, &x));
    assert(ores_execution_enter(&a, 1, 2, 1, &x));
    assert(!ores_execution_retired(&a, 1));
    assert(ores_execution_leave(&a, x, 0));
    assert(ores_execution_retired(&a, 1));
    assert(ores_execution_restart(&a) == 2);
    ores_execution_lock(&a);
    a.sequence = UINT64_MAX; /* deterministic test-only exhaustion */
    ores_execution_unlock(&a);
    assert(!ores_execution_enter(&a, 2, 3, 0, &x));
    assert(ores_execution_stop(&a, 2));
    assert(ores_execution_retired(&a, 2));
    assert(ores_execution_restart(&a) == 0);
    assert(ores_execution_destroy(&a));
}
int main(void) {
    concurrent_carriers();
    suspend_stop_restart();
    preadmitted_drain_and_exhaustion();
    puts("ORES_NATIVE_SUPERVISOR_EXECUTION_AUTHORITY_PASS");
    return 0;
}
