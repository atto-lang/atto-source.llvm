#include "ControlPlaneArena32.h"
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

/* Real concurrent allocator exercise: no guest runtime or TSAN claim. */
typedef struct {
    AttoControlPlaneArena32 *arena;
    atomic_int *go;
    int id;
    int failed;
} ThreadProbe;
static void *run_control_worker(void *arg) {
    ThreadProbe *probe = arg;
    while (!atomic_load_explicit(probe->go, memory_order_acquire)) { }
    for (int i = 0; i < 1024; ++i) {
        uint64_t ref = 0;
        if (!atto_control32_allocate(probe->arena, &ref)) { probe->failed = 1; return NULL; }
        const int64_t value = ((int64_t)probe->id << 32) | (int64_t)i;
        int64_t actual = 0;
        if (!atto_control32_write_i64(probe->arena, ref, 0, value) ||
            !atto_control32_read_i64(probe->arena, ref, 0, &actual) || actual != value ||
            !atto_control32_drop(probe->arena, ref) ||
            atto_control32_write_i64(probe->arena, ref, 0, value)) {
            probe->failed = 1;
            return NULL;
        }
    }
    return NULL;
}

typedef struct {
    AttoControlPlaneArena32 *arena;
    uint64_t witness;
    atomic_int ready;
    int invalid;
} PairProbe;
static void *write_pair_worker(void *raw) {
    PairProbe *p = raw;
    atomic_store_explicit(&p->ready, 1, memory_order_release);
    for (uint64_t i = 0; i < 32768; ++i) {
        if (!atto_control32_write_i128(p->arena, p->witness, i, ~i)) {
            p->invalid = 1;
            break;
        }
    }
    return NULL;
}
/* Reproducible model check of real native slot ownership. The PR host ABI is
 * intentionally NOT a compiled Atto actor. Randomized operation order catches
 * ABA, aliasing and stale/cross-domain successes that individual examples miss.
 * A fixed seed keeps failures replayable across GCC and Clang sanitizers. */
static uint64_t next_random(uint64_t *seed) {
    uint64_t x = *seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *seed = x;
    return x;
}
static void test_randomized_control_lifetimes(void) {
    enum { SLOTS = 32, STEPS = 50000 };
    AttoControlPlaneArena32 *a = atto_control32_new(SLOTS);
    AttoControlPlaneArena32 *foreign_arena = atto_control32_new(SLOTS);
    CHECK(a && foreign_arena);
    uint64_t foreign = 0;
    CHECK(atto_control32_allocate(foreign_arena, &foreign));
    uint64_t active[SLOTS] = {0};
    uint64_t retired[SLOTS] = {0};
    uint64_t expected_low[SLOTS] = {0};
    uint64_t seed = UINT64_C(0x9e3779b97f4a7c15);
    unsigned live = 0, allocations = 0, drops = 0;
    for (unsigned step = 0; step < STEPS; ++step) {
        uint64_t r = next_random(&seed);
        if ((r & 3u) != 0u && live < SLOTS) {
            uint64_t current = 0;
            CHECK(atto_control32_allocate(a, &current));
            uint32_t slot = ((uint32_t)current & ((1u << 20) - 1u)) - 1u;
            CHECK(slot < SLOTS && active[slot] == 0);
            CHECK(current != retired[slot]);
            active[slot] = current;
            ++live;
            ++allocations;
            expected_low[slot] = next_random(&seed);
            CHECK(atto_control32_write_i128(a, current, expected_low[slot], ~expected_low[slot]));
            uint64_t low = UINT64_MAX, high = UINT64_MAX;
            CHECK(atto_control32_read_i128(a, current, &low, &high));
            CHECK(low == expected_low[slot] && high == ~low);
            CHECK(!atto_control32_read_i128(foreign_arena, current, &low, &high));
            CHECK(low == 0 && high == 0);
            CHECK(!atto_control32_write_i128(a, foreign, 1u, 2u));
            if (retired[slot]) {
                CHECK(!atto_control32_write_i128(a, retired[slot], 7u, 8u));
                CHECK(!atto_control32_read_i128(a, retired[slot], &low, &high));
                CHECK(low == 0 && high == 0);
            }
        } else if (live) {
            uint32_t start = (uint32_t)(r % SLOTS);
            for (uint32_t n = 0; n < SLOTS; ++n) {
                uint32_t slot = (start + n) % SLOTS;
                if (!active[slot]) continue;
                uint64_t low = 0, high = 0;
                CHECK(atto_control32_read_i128(a, active[slot], &low, &high));
                CHECK(low == expected_low[slot] && high == ~expected_low[slot]);
                uint64_t prior = active[slot];
                CHECK(atto_control32_drop(a, prior));
                CHECK(!atto_control32_drop(a, prior));
                CHECK(!atto_control32_read_i128(a, prior, &low, &high));
                CHECK(low == 0 && high == 0);
                retired[slot] = prior;
                active[slot] = 0;
                --live;
                ++drops;
                break;
            }
        } else {
            /* When fully occupied, no other slot may be silently admitted. */
            uint64_t denied = UINT64_MAX;
            CHECK(live == SLOTS);
            CHECK(!atto_control32_allocate(a, &denied) && denied == 0);
        }
    }
    for (uint32_t slot = 0; slot < SLOTS; ++slot) {
        if (active[slot]) {
            uint64_t low = 0, high = 0;
            CHECK(atto_control32_read_i128(a, active[slot], &low, &high));
            CHECK(low == expected_low[slot] && high == ~low);
            CHECK(atto_control32_drop(a, active[slot]));
            ++drops;
            --live;
        }
    }
    CHECK(!live && allocations == drops && allocations > 1000);
    CHECK(atto_control32_drop(foreign_arena, foreign));
    atto_control32_retire(a);
    atto_control32_retire(foreign_arena);
    atto_control32_destroy(a);
    atto_control32_destroy(foreign_arena);
    puts("PASS llvm control deterministic 50000-op native lifetime model (NOT compiled Atto)");
}

int main(void) {
    CHECK(strcmp(ATTO_CONTROL_REF32_ABI, "control_generation12_slot20_v1") == 0);
    CHECK(ATTO_CONTROL_RECORD_BYTES == 32u);
#ifdef ATTO_CONTROL32_TESTING
    uint32_t issuer_probe = UINT32_MAX;
    CHECK(atto_control32_test_claim_issuer(0, &issuer_probe) && issuer_probe == 1u);
    CHECK(atto_control32_test_claim_issuer(UINT32_MAX - 1u, &issuer_probe)
          && issuer_probe == UINT32_MAX);
    CHECK(!atto_control32_test_claim_issuer(UINT32_MAX, &issuer_probe)
          && issuer_probe == 0);
    CHECK(!atto_control32_test_claim_issuer(UINT64_MAX, &issuer_probe)
          && issuer_probe == 0);
    CHECK(!atto_control32_test_claim_issuer(UINT64_MAX - 1u, &issuer_probe)
          && issuer_probe == 0);
    CHECK(!atto_control32_test_claim_issuer(0, NULL));
#endif
    AttoControlPlaneArena32 *a = atto_control32_new(2), *b = atto_control32_new(2);
    CHECK(a && b);
    uint64_t parent = 0, child = 0, foreign = 0, value = 0;
    int64_t scalar = 0;
    CHECK(atto_control32_allocate(a, &parent));
    CHECK(atto_control32_allocate(a, &child));
    CHECK(((uint32_t)parent & ((1u << 20) - 1u)) == 1u);
    CHECK(((uint32_t)parent >> 20) == 1u);
    CHECK(atto_control32_allocate(b, &foreign));
    CHECK((uint32_t)parent == (uint32_t)foreign);
    CHECK(!atto_control32_read_i64(b, parent, 0, &scalar));
    CHECK(!atto_control32_write_ref(a, parent, foreign));
    CHECK(atto_control32_write_i64(a, parent, 0, INT64_C(9000000000)));
    CHECK(atto_control32_read_i64(a, parent, 0, &scalar) && scalar == INT64_C(9000000000));
    CHECK(atto_control32_write_i64(a, parent, 8, INT64_MIN));
    CHECK(atto_control32_read_i64(a, parent, 8, &scalar) && scalar == INT64_MIN);
    /* One 32-bit locator to a coherent 128-bit value (two 64-bit limbs). */
    uint64_t low = 0, high = 0;
    CHECK(atto_control32_write_i128(a, parent, UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)));
    CHECK(atto_control32_read_i128(a, parent, &low, &high));
    CHECK(low == UINT64_C(0x0123456789abcdef) && high == UINT64_C(0xfedcba9876543210));
    CHECK(!atto_control32_read_i128(a, parent, &low, &low));
    CHECK(!atto_control32_read_i128(b, parent, &low, &high));
    CHECK(low == 0 && high == 0);
    CHECK(!atto_control32_read_i128(a, parent, NULL, &high));
    CHECK(atto_control32_write_ref(a, parent, child));
    CHECK(atto_control32_read_ref(a, parent, &value) && value == child);
    CHECK(!atto_control32_write_i64(a, parent, 16, 1));
    value = UINT64_MAX;
    CHECK(!atto_control32_allocate(a, &value) && value == 0);
    CHECK(atto_control32_drop(a, child));
    CHECK(!atto_control32_read_i64(a, child, 0, &scalar));
    value = UINT64_MAX;
    CHECK(!atto_control32_read_ref(a, parent, &value) && value == 0);
    uint64_t replacement = 0;
    CHECK(atto_control32_allocate(a, &replacement) && replacement != child);
    CHECK(atto_control32_write_ref(a, parent, replacement));
    CHECK(atto_control32_read_ref(a, parent, &value) && value == replacement);
    CHECK(atto_control32_drop(a, parent));
    CHECK(atto_control32_drop(a, replacement));
    CHECK(!atto_control32_drop(a, replacement));
    AttoControlPlaneArena32 *limited = atto_control32_new(1);
    CHECK(limited);
    uint64_t token = 0;
    CHECK(atto_control32_allocate(limited, &token));
    for (int i = 1; i < 4095; ++i) {
        CHECK(atto_control32_drop(limited, token));
        CHECK(atto_control32_allocate(limited, &token));
    }
    CHECK(((uint32_t)token >> 20) == 4095u);
    CHECK(atto_control32_drop(limited, token));
    CHECK(!atto_control32_allocate(limited, &token) && token == 0);
    CHECK(atto_control32_read_i64(b, foreign, 0, &scalar) && scalar == 0);
    atto_control32_retire(a);
    atto_control32_retire(a);
    CHECK(!atto_control32_write_i64(a, parent, 0, 1));
    atto_control32_destroy(limited);
    atto_control32_destroy(b);
    atto_control32_destroy(a);
    AttoControlPlaneArena32 *concurrent = atto_control32_new(8);
    CHECK(concurrent);
    atomic_int go = ATOMIC_VAR_INIT(0);
    pthread_t workers[4];
    ThreadProbe args[4] = {{0}};
    for (int i = 0; i < 4; ++i) {
        args[i].arena = concurrent;
        args[i].go = &go;
        args[i].id = i + 1;
        CHECK(pthread_create(&workers[i], NULL, run_control_worker, &args[i]) == 0);
    }
    atomic_store_explicit(&go, 1, memory_order_release);
    for (int i = 0; i < 4; ++i) {
        CHECK(pthread_join(workers[i], NULL) == 0);
        CHECK(args[i].failed == 0);
    }
    atto_control32_retire(concurrent);
    uint64_t denied = UINT64_MAX;
    CHECK(!atto_control32_allocate(concurrent, &denied) && denied == 0);
    atto_control32_destroy(concurrent);
    AttoControlPlaneArena32 *pair_arena = atto_control32_new(1);
    CHECK(pair_arena);
    uint64_t pair_ref = 0;
    CHECK(atto_control32_allocate(pair_arena, &pair_ref));
    CHECK(atto_control32_write_i128(pair_arena, pair_ref, 0, UINT64_MAX));
    PairProbe pair = {.arena = pair_arena, .witness = pair_ref, .ready = ATOMIC_VAR_INIT(0)};
    pthread_t pair_writer;
    CHECK(pthread_create(&pair_writer, NULL, write_pair_worker, &pair) == 0);
    while (!atomic_load_explicit(&pair.ready, memory_order_acquire)) { }
    for (int i = 0; i < 32768; ++i) {
        uint64_t observed_low = 0, observed_high = 0;
        CHECK(atto_control32_read_i128(pair_arena, pair_ref, &observed_low, &observed_high));
        CHECK(observed_high == ~observed_low); /* no torn 128-bit pair */
    }
    CHECK(pthread_join(pair_writer, NULL) == 0 && pair.invalid == 0);
    CHECK(atto_control32_drop(pair_arena, pair_ref));
    uint64_t denied_low = UINT64_MAX, denied_high = UINT64_MAX;
    CHECK(!atto_control32_read_i128(pair_arena, pair_ref, &denied_low, &denied_high));
    CHECK(denied_low == 0 && denied_high == 0);
    atto_control32_destroy(pair_arena);
    test_randomized_control_lifetimes();
    puts("PASS llvm control i128 coherent concurrent host (NOT compiled Atto)");
    puts("PASS control-plane-ref32 llvm C11 native host 12 guarded scenarios; NO compiled Atto guest or zero-GC proof");
    return 0;
}
