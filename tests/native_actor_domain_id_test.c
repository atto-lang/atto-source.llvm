#include "oreslang/runtime/NativeActorDomainId.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define WORKERS 12
#define ATTEMPTS 128
#define LAST_IDS 8

static _Atomic(uint64_t) shared_next;
static _Atomic(unsigned) issued_count;
static uint64_t issued[LAST_IDS];

static void fail(const char* reason) {
    fprintf(stderr, "FAIL native actor ID exhaustion: %s\n", reason);
    exit(1);
}

static void* contender(void* unused) {
    (void)unused;
    for (unsigned attempt = 0; attempt < ATTEMPTS; ++attempt) {
        const uint64_t id = ores_native_actor_domain_reserve(&shared_next);
        if (id == 0) continue;
        const unsigned index = atomic_fetch_add_explicit(
            &issued_count, 1, memory_order_relaxed);
        if (index >= LAST_IDS) fail("issued more identities than remain");
        issued[index] = id;
    }
    return NULL;
}

int main(void) {
    atomic_init(&shared_next, UINT64_MAX - LAST_IDS);
    atomic_init(&issued_count, 0);

    pthread_t threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) {
        if (pthread_create(&threads[i], NULL, contender, NULL))
            fail("failed to create contention thread");
    }
    for (unsigned i = 0; i < WORKERS; ++i) {
        if (pthread_join(threads[i], NULL)) fail("failed to join contender");
    }
    if (atomic_load_explicit(&issued_count, memory_order_relaxed) != LAST_IDS)
        fail("did not allocate every remaining unique identity");

    for (unsigned i = 0; i < LAST_IDS; ++i) {
        if (issued[i] < UINT64_MAX - LAST_IDS || issued[i] == UINT64_MAX)
            fail("invalid identity in terminal interval");
        for (unsigned j = i + 1; j < LAST_IDS; ++j) {
            if (issued[i] == issued[j]) fail("duplicate domain issued");
        }
    }

    if (atomic_load_explicit(&shared_next, memory_order_relaxed) != UINT64_MAX)
        fail("domain counter wrapped beyond terminal sentinel");
    for (unsigned i = 0; i < 1024; ++i) {
        if (ores_native_actor_domain_reserve(&shared_next))
            fail("terminal state reissued a previously used domain");
    }

    _Atomic(uint64_t) zero = 0;
    if (ores_native_actor_domain_reserve(&zero) != 0
            || atomic_load_explicit(&zero, memory_order_relaxed) != 0)
        fail("zero was minted or advanced");

    _Atomic(uint64_t) first = 1;
    if (ores_native_actor_domain_reserve(&first) != 1
            || atomic_load_explicit(&first, memory_order_relaxed) != 2)
        fail("ordinary nonterminal allocation changed");

    puts("PASS native actor domain IDs: concurrent terminal exhaustion, no wrap/ABA");
    return 0;
}
