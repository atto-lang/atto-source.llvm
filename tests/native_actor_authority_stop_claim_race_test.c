#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeExecutionAuthority.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { ROUNDS = 1000 };
#define CHECK(condition) do { if (!(condition)) abort(); } while (0)

typedef struct {
    OresNativeExecutionAuthority* authority;
    OresExecutionClaim stolen;
    _Atomic int examined;
    _Atomic int violations;
} Race;

static void* try_steal_and_stop(void* context) {
    Race* race = (Race*)context;
    /* A copied claim cannot be used to own or release an active actor turn
     * from a different carrier, even when the token and generation match. */
    if (ores_execution_owns(race->authority, race->stolen))
        atomic_fetch_add(&race->violations, 1);
    if (ores_execution_leave(race->authority, race->stolen, 0))
        atomic_fetch_add(&race->violations, 1);
    if (ores_execution_retired(race->authority, race->stolen.generation))
        atomic_fetch_add(&race->violations, 1);
    if (!ores_execution_stop(race->authority, race->stolen.generation))
        atomic_fetch_add(&race->violations, 1);
    atomic_store_explicit(&race->examined, 1, memory_order_release);
    return NULL;
}

static void run_round(void) {
    OresNativeExecutionAuthority authority;
    CHECK(ores_execution_init(&authority));
    OresExecutionClaim claimed = {0};
    CHECK(ores_execution_enter(&authority, 1, UINT64_C(0xdeadbeef), 0, &claimed));
    CHECK(ores_execution_owns(&authority, claimed));

    Race race = {.authority = &authority, .stolen = claimed};
    pthread_t interloper;
    CHECK(pthread_create(&interloper, NULL, try_steal_and_stop, &race) == 0);
    int completed = 0;
    for (unsigned attempt = 0; attempt < 1000000; ++attempt) {
        if (atomic_load_explicit(&race.examined, memory_order_acquire)) {
            completed = 1;
            break;
        }
        sched_yield();
    }
    CHECK(pthread_join(interloper, NULL) == 0);
    CHECK(completed);
    CHECK(atomic_load(&race.violations) == 0);
    CHECK(ores_execution_phase(&authority) == ORES_EXEC_STOPPING);
    CHECK(ores_execution_owns(&authority, claimed));
    CHECK(!ores_execution_restart(&authority));
    CHECK(!ores_execution_retired(&authority, 1));
    CHECK(!ores_execution_enter(&authority, 1, UINT64_C(0x22), 1,
                                &(OresExecutionClaim){0}));
    CHECK(ores_execution_leave(&authority, claimed, 1));
    CHECK(!ores_execution_leave(&authority, claimed, 0));
    CHECK(ores_execution_retired(&authority, 1));
    CHECK(ores_execution_phase(&authority) == ORES_EXEC_TERMINATED);
    CHECK(!ores_execution_enter(&authority, 1, UINT64_C(0x33), 1,
                                &(OresExecutionClaim){0}));
    CHECK(!ores_execution_stop(&authority, 1));

    const uint64_t fresh_generation = ores_execution_restart(&authority);
    CHECK(fresh_generation == 2);
    CHECK(!ores_execution_owns(&authority, claimed));
    CHECK(!ores_execution_enter(&authority, 1, UINT64_C(0x44), 0,
                                &(OresExecutionClaim){0}));
    CHECK(!ores_execution_stop(&authority, 1));
    CHECK(ores_execution_phase(&authority) == ORES_EXEC_READY);

    /* After STOPPING, only a turn already accepted by its scheduler may be
     * entered; unadmitted work cannot steal the actor's execution claim. */
    CHECK(ores_execution_stop(&authority, fresh_generation));
    CHECK(!ores_execution_enter(&authority, fresh_generation,
                                UINT64_C(0x55), 0, &(OresExecutionClaim){0}));
    OresExecutionClaim admitted = {0};
    CHECK(ores_execution_enter(&authority, fresh_generation,
                               UINT64_C(0x66), 1, &admitted));
    CHECK(ores_execution_owns(&authority, admitted));
    CHECK(!ores_execution_retired(&authority, fresh_generation));
    CHECK(ores_execution_leave(&authority, admitted, 0));
    CHECK(ores_execution_retired(&authority, fresh_generation));
    CHECK(ores_execution_destroy(&authority));
}

int main(void) {
    for (int round = 0; round < ROUNDS; ++round) run_round();
    puts("NATIVE_ACTOR_AUTHORITY_STOP_CLAIM_RACE_PASS rounds=1000 stolen_claim_denied=1 retirement_fenced=1 stale_generation_denied=1 admitted_stop_drains=1");
    return 0;
}
