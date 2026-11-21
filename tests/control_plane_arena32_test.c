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

int main(void) {
    CHECK(strcmp(ATTO_CONTROL_REF32_ABI, "control_generation12_slot20_v1") == 0);
    CHECK(ATTO_CONTROL_RECORD_BYTES == 32u);
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
    puts("PASS control-plane-ref32 llvm C11 native host 12 guarded scenarios; NO compiled Atto guest or zero-GC proof");
    return 0;
}
