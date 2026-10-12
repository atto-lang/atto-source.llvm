#include "oreslang/runtime/NativeExecutionAuthority.h"
#include <stdlib.h>
/* Safety assertions must never disappear in NDEBUG/Release builds. */
#define REQUIRE(condition) do { if (!(condition)) abort(); } while (0)
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
        REQUIRE(ores_execution_owns(a->authority, claim));
        REQUIRE(ores_execution_leave(a->authority, claim, 0));
        REQUIRE(!ores_execution_leave(a->authority, claim, 0));
    }
    return NULL;
}

static void concurrent_carriers(void) {
    OresNativeExecutionAuthority authority;
    REQUIRE(ores_execution_init(&authority));
    _Atomic int entered = 0, release = 0, winners = 0;
    ClaimArg args[CARRIERS];
    pthread_t threads[CARRIERS];
    for (int i = 0; i < CARRIERS; ++i) {
        args[i] = (ClaimArg){&authority, &entered, &release, &winners, (uint64_t)(i + 1)};
        REQUIRE(pthread_create(&threads[i], NULL, contender, &args[i]) == 0);
    }
    while (atomic_load(&entered) != CARRIERS) sched_yield();
    REQUIRE(atomic_load(&winners) == 1);
    REQUIRE(ores_execution_phase(&authority) == ORES_EXEC_RUNNING);
    OresExecutionClaim forbidden = {0};
    REQUIRE(!ores_execution_enter(&authority, 1, 99, 0, &forbidden));
    atomic_store(&release, 1);
    for (int i = 0; i < CARRIERS; ++i)
        REQUIRE(pthread_join(threads[i], NULL) == 0);
    REQUIRE(ores_execution_phase(&authority) == ORES_EXEC_READY);
    REQUIRE(ores_execution_destroy(&authority));
}
static void suspend_stop_restart(void) {
    OresNativeExecutionAuthority a;
    REQUIRE(ores_execution_init(&a));
    OresExecutionClaim x={0}, y={0};
    REQUIRE(ores_execution_enter(&a, 1, 7, 0, &x));
    REQUIRE(!ores_execution_enter(&a, 1, 8, 0, &y));
    REQUIRE(ores_execution_leave(&a, x, 1));
    REQUIRE(ores_execution_phase(&a) == ORES_EXEC_SUSPENDED);
    REQUIRE(ores_execution_enter(&a, 1, 8, 0, &y));
    REQUIRE(!ores_execution_leave(&a, x, 0));
    REQUIRE(!ores_execution_retired(&a, 1));
    REQUIRE(ores_execution_stop(&a, 1));
    REQUIRE(!ores_execution_retired(&a, 1));
    REQUIRE(!ores_execution_enter(&a, 1, 9, 0, &x));
    REQUIRE(ores_execution_leave(&a, y, 0));
    REQUIRE(ores_execution_retired(&a, 1));
    REQUIRE(!ores_execution_retired(&a, 1));
    REQUIRE(ores_execution_restart(&a) == 2);
    REQUIRE(!ores_execution_enter(&a, 1, 8, 0, &x));
    REQUIRE(ores_execution_enter(&a, 2, 9, 0, &x));
    REQUIRE(!ores_execution_leave(&a, y, 0));
    REQUIRE(ores_execution_leave(&a, x, 0));
    REQUIRE(ores_execution_stop(&a, 2));
    REQUIRE(ores_execution_retired(&a, 2));
    REQUIRE(ores_execution_destroy(&a));
}
/* A copied capability cannot release another worker's physical actor
 * lease, even if the caller knows the correct generation, sequence and
 * carrier token. The rightful owner must remain active after the attempt. */
typedef struct {
    OresNativeExecutionAuthority* authority;
    OresExecutionClaim stolen;
    int foreign_owns;
    int foreign_released;
} ForeignClaimAttempt;
static void* attempt_foreign_release(void* raw) {
    ForeignClaimAttempt* attempt = (ForeignClaimAttempt*)raw;
    attempt->foreign_owns = ores_execution_owns(attempt->authority, attempt->stolen);
    attempt->foreign_released =
        ores_execution_leave(attempt->authority, attempt->stolen, 0);
    return NULL;
}
static void foreign_claim_release_is_forbidden(void) {
    OresNativeExecutionAuthority authority;
    REQUIRE(ores_execution_init(&authority));
    OresExecutionClaim rightful = {0};
    REQUIRE(ores_execution_enter(&authority, 1, 17, 0, &rightful));
    ForeignClaimAttempt attempt = {&authority, rightful, 0, 0};
    pthread_t other;
    REQUIRE(pthread_create(&other, NULL, attempt_foreign_release, &attempt) == 0);
    REQUIRE(pthread_join(other, NULL) == 0);
    REQUIRE(!attempt.foreign_owns && !attempt.foreign_released);
    REQUIRE(ores_execution_owns(&authority, rightful));
    OresExecutionClaim blocked = {0};
    REQUIRE(!ores_execution_enter(&authority, 1, 18, 0, &blocked));
    REQUIRE(ores_execution_leave(&authority, rightful, 0));
    REQUIRE(ores_execution_stop(&authority, 1));
    REQUIRE(ores_execution_retired(&authority, 1));
    REQUIRE(ores_execution_destroy(&authority));
}

static void preadmitted_drain_and_exhaustion(void) {
    OresNativeExecutionAuthority a;
    REQUIRE(ores_execution_init(&a));
    REQUIRE(ores_execution_stop(&a, 1));
    OresExecutionClaim x={0};
    REQUIRE(!ores_execution_enter(&a, 1, 2, 0, &x));
    REQUIRE(ores_execution_enter(&a, 1, 2, 1, &x));
    REQUIRE(!ores_execution_retired(&a, 1));
    REQUIRE(ores_execution_leave(&a, x, 0));
    REQUIRE(ores_execution_retired(&a, 1));
    REQUIRE(ores_execution_restart(&a) == 2);
    ores_execution_lock(&a);
    a.sequence = UINT64_MAX; /* deterministic test-only exhaustion */
    ores_execution_unlock(&a);
    REQUIRE(!ores_execution_enter(&a, 2, 3, 0, &x));
    REQUIRE(ores_execution_stop(&a, 2));
    REQUIRE(ores_execution_retired(&a, 2));
    REQUIRE(ores_execution_restart(&a) == 0);
    REQUIRE(ores_execution_destroy(&a));
}
int main(void) {
    concurrent_carriers();
    suspend_stop_restart();
    preadmitted_drain_and_exhaustion();
    foreign_claim_release_is_forbidden();
    puts("ORES_NATIVE_SUPERVISOR_EXECUTION_AUTHORITY_PASS");
    return 0;
}
