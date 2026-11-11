/* Host-only lifecycle evidence for actor-memory v1.
 * NOT guest source lowering, NativeAOT, a GC certificate, or a hard-RT proof. */
#include "oreslang/runtime/ActorArena.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Linux link-time instrumentation counts allocator calls originating in
 * the linked test/ActorArena translation units during admitted actor turns.
 * This does not intercept every possible libc-internal/FFI allocation. */
#if defined(__linux__) && defined(ORESLANG_TEST_WRAP_ALLOC)
#include <stdatomic.h>
static _Atomic unsigned int count_during_turn = 0;
static _Atomic unsigned long long alloc_calls_during_turn = 0;
extern void* __real_malloc(size_t bytes);
extern void* __real_calloc(size_t count, size_t bytes);
extern void* __real_realloc(void* old, size_t bytes);
void* __wrap_malloc(size_t bytes) {
    if (atomic_load_explicit(&count_during_turn, memory_order_relaxed))
        atomic_fetch_add_explicit(&alloc_calls_during_turn, 1, memory_order_relaxed);
    return __real_malloc(bytes);
}
void* __wrap_calloc(size_t count, size_t bytes) {
    if (atomic_load_explicit(&count_during_turn, memory_order_relaxed))
        atomic_fetch_add_explicit(&alloc_calls_during_turn, 1, memory_order_relaxed);
    return __real_calloc(count, bytes);
}
void* __wrap_realloc(void* old, size_t bytes) {
    if (atomic_load_explicit(&count_during_turn, memory_order_relaxed))
        atomic_fetch_add_explicit(&alloc_calls_during_turn, 1, memory_order_relaxed);
    return __real_realloc(old, bytes);
}
#endif

#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "ACTOR_MEMORY_HOST_V1_FAIL line=%d: %s\n", __LINE__, #expr); \
    return 1; } } while (0)

static int positive_lifecycle(void) {
    OresActorArena* arena = ores_actor_arena_open(101, 7, 4, 64, 256);
    REQUIRE(arena != NULL);
    REQUIRE(ores_actor_arena_prefault(arena) == 1);
    REQUIRE(ores_actor_arena_is_prefaulted(arena) == 1);
    OresArenaTurn initial = ores_actor_arena_enter_realtime(arena, 101, 7);
    OresArenaHandle state = ores_actor_arena_alloc(arena, initial, sizeof(uint64_t));
    uint64_t value = 42;
    ores_actor_arena_write(arena, initial, state, 0, &value, sizeof(value));
    ores_actor_arena_leave(arena, initial);

#if defined(__linux__) && defined(ORESLANG_TEST_WRAP_ALLOC)
    atomic_store_explicit(&alloc_calls_during_turn, 0, memory_order_relaxed);
    atomic_store_explicit(&count_during_turn, 1, memory_order_release);
#endif
    for (uint64_t i = 0; i < 1024; ++i) {
        OresArenaTurn turn = ores_actor_arena_enter_realtime(arena, 101, 7);
        value = 0;
        ores_actor_arena_read(arena, turn, state, 0, &value, sizeof(value));
        REQUIRE(value == 42 + i);
        OresArenaScratch scratch = ores_actor_arena_scratch_alloc(
            arena, turn, sizeof(uint64_t), 8);
        uint64_t clean = UINT64_MAX;
        ores_actor_arena_scratch_read(arena, turn, scratch, 0, &clean, sizeof(clean));
        REQUIRE(clean == 0);
        value++;
        ores_actor_arena_write(arena, turn, state, 0, &value, sizeof(value));
        ores_actor_arena_scratch_write(arena, turn, scratch, 0, &value, sizeof(value));
        OresArenaHandle temporary = ores_actor_arena_alloc(arena, turn, 8);
        ores_actor_arena_drop(arena, turn, temporary);
        ores_actor_arena_leave(arena, turn);
    }
#if defined(__linux__) && defined(ORESLANG_TEST_WRAP_ALLOC)
    atomic_store_explicit(&count_during_turn, 0, memory_order_release);
    REQUIRE(atomic_load_explicit(&alloc_calls_during_turn, memory_order_relaxed) == 0);
#endif
    OresArenaTurn final = ores_actor_arena_enter(arena, 101, 7);
    value = 0;
    ores_actor_arena_read(arena, final, state, 0, &value, sizeof(value));
    REQUIRE(value == 42 + 1024);
    OresArenaLoan pin = ores_actor_arena_borrow(arena, final, state);
    ores_actor_arena_leave(arena, final);
    ores_actor_arena_retire(arena);
    REQUIRE(ores_actor_arena_try_destroy(arena) == 0);
    ores_actor_arena_release(arena, pin);
    REQUIRE(ores_actor_arena_try_destroy(arena) == 1);
    return 0;
}

#if defined(__unix__) || defined(__APPLE__)
static void invalid_operation(int which) {
    OresActorArena* a = ores_actor_arena_open(111, 1, 2, 64, 128);
    OresArenaTurn t = ores_actor_arena_enter(a, 111, 1);
    OresArenaHandle h = ores_actor_arena_alloc(a, t, 8);
    uint64_t value = 0;
    if (which == 0) {
        ores_actor_arena_drop(a, t, h);
        (void)ores_actor_arena_alloc(a, t, 8);
        ores_actor_arena_read(a, t, h, 0, &value, sizeof(value));
    } else if (which == 1) {
        ores_actor_arena_leave(a, t);
        OresActorArena* b = ores_actor_arena_open(112, 1, 2, 64, 128);
        OresArenaTurn bt = ores_actor_arena_enter(b, 112, 1);
        ores_actor_arena_read(b, bt, h, 0, &value, sizeof(value));
    } else {
        OresArenaScratch scratch = ores_actor_arena_scratch_alloc(a, t, 8, 8);
        ores_actor_arena_leave(a, t);
        OresArenaTurn next = ores_actor_arena_enter(a, 111, 1);
        ores_actor_arena_scratch_read(a, next, scratch, 0, &value, sizeof(value));
    }
}

static int must_abort(int which) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        invalid_operation(which);
        _exit(97);
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid
        && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    REQUIRE(positive_lifecycle() == 0);
#if defined(__unix__) || defined(__APPLE__)
    REQUIRE(must_abort(0)); /* stale after drop + slot reuse */
    REQUIRE(must_abort(1)); /* different actor's domain */
    REQUIRE(must_abort(2)); /* scratch after turn exit */
#else
    puts("HOST_NEGATIVE_CONTROLS_NOT_EXECUTED=non-POSIX");
#endif
#if defined(__linux__) && defined(ORESLANG_TEST_WRAP_ALLOC)
    puts("ACTOR_MEMORY_E2E_V1_LINKED_TURN_ALLOC_CALLS=0");
#else
    puts("ACTOR_MEMORY_E2E_V1_LINKED_TURN_ALLOC_CALLS=NOT_MEASURED");
#endif
    puts("ACTOR_MEMORY_E2E_V1_HOST_ONLY=PASS");
    puts("ACTOR_MEMORY_E2E_V1_COMPILED_GUEST=NOT_EXECUTED");
    return 0;
}
