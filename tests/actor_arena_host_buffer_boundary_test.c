/* Host-only negative oracle: the C actor arena must reject aliasing and
 * uintptr-wrapping copy buffers BEFORE memcpy or allocator mutation.
 * This uses the real production ActorArena.c, not a model. */
#include "oreslang/runtime/ActorArena.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "host buffer boundary failure at %d\n", __LINE__); return 1; } } while (0)

enum BadCopy {
    READ_CONTROL, WRITE_CONTROL, SCRATCH_READ_CONTROL,
    SCRATCH_WRITE_CONTROL, READ_CONTROL_INTERIOR,
    SCRATCH_READ_CONTROL_INTERIOR, READ_WRAP, WRITE_WRAP,
    SCRATCH_READ_WRAP, SCRATCH_WRITE_WRAP, BAD_COPY_COUNT
};

#if defined(__unix__) || defined(__APPLE__)
static void unsafe_copy(enum BadCopy kind) {
    OresActorArena* a = ores_actor_arena_open(90, 7, 2, 64, 128);
    OresArenaTurn turn = ores_actor_arena_enter(a, 90, 7);
    OresArenaHandle slot = ores_actor_arena_alloc(a, turn, 16);
    unsigned char sample[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ores_actor_arena_write(a, turn, slot, 0, sample, sizeof(sample));
    OresArenaScratch scratch = ores_actor_arena_scratch_alloc(
        a, turn, sizeof(sample), 8);
    ores_actor_arena_scratch_write(a, turn, scratch, 0, sample, sizeof(sample));
    unsigned char* control = (unsigned char*)a;
    void* wrapping = (void*)(uintptr_t)(UINTPTR_MAX - 3u);
    switch (kind) {
        case READ_CONTROL:
            ores_actor_arena_read(a, turn, slot, 0, control, 8); break;
        case WRITE_CONTROL:
            ores_actor_arena_write(a, turn, slot, 0, control, 8); break;
        case SCRATCH_READ_CONTROL:
            ores_actor_arena_scratch_read(a, turn, scratch, 0, control, 8); break;
        case SCRATCH_WRITE_CONTROL:
            ores_actor_arena_scratch_write(a, turn, scratch, 0, control, 8); break;
        case READ_CONTROL_INTERIOR:
            ores_actor_arena_read(a, turn, slot, 0, control + 4, 8); break;
        case SCRATCH_READ_CONTROL_INTERIOR:
            ores_actor_arena_scratch_read(a, turn, scratch, 0, control + 4, 8); break;
        case READ_WRAP:
            ores_actor_arena_read(a, turn, slot, 0, wrapping, 8); break;
        case WRITE_WRAP:
            ores_actor_arena_write(a, turn, slot, 0, wrapping, 8); break;
        case SCRATCH_READ_WRAP:
            ores_actor_arena_scratch_read(a, turn, scratch, 0, wrapping, 8); break;
        case SCRATCH_WRITE_WRAP:
            ores_actor_arena_scratch_write(a, turn, scratch, 0, wrapping, 8); break;
        case BAD_COPY_COUNT: break;
    }
}
static int expects_abort(enum BadCopy kind) {
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &core);
        unsafe_copy(kind);
        _exit(90); /* an unguarded memcpy is a test failure */
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid &&
           WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    OresActorArena* a = ores_actor_arena_open(91, 8, 2, 64, 128);
    OresArenaTurn turn = ores_actor_arena_enter(a, 91, 8);
    OresArenaHandle slot = ores_actor_arena_alloc(a, turn, 16);
    unsigned char value[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    unsigned char actual[8] = {0};
    ores_actor_arena_write(a, turn, slot, 0, value, sizeof(value));
    ores_actor_arena_read(a, turn, slot, 0, actual, sizeof(actual));
    for (size_t i = 0; i < sizeof(value); ++i) CHECK(actual[i] == value[i]);
    OresArenaScratch scratch = ores_actor_arena_scratch_alloc(
        a, turn, sizeof(value), 8);
    ores_actor_arena_scratch_write(a, turn, scratch, 0, value, sizeof(value));
    for (size_t i = 0; i < sizeof(actual); ++i) actual[i] = 0;
    ores_actor_arena_scratch_read(a, turn, scratch, 0, actual, sizeof(actual));
    for (size_t i = 0; i < sizeof(value); ++i) CHECK(actual[i] == value[i]);

    /* Zero bytes never access the supplied address, including protected ones. */
    ores_actor_arena_read(a, turn, slot, 0, a, 0);
    ores_actor_arena_write(a, turn, slot, 0, a, 0);
    ores_actor_arena_scratch_read(a, turn, scratch, 0, a, 0);
    ores_actor_arena_scratch_write(a, turn, scratch, 0, a, 0);
    ores_actor_arena_leave(a, turn);
    ores_actor_arena_retire(a);
    CHECK(ores_actor_arena_try_destroy(a) == 1);
#if defined(__unix__) || defined(__APPLE__)
    for (int i = READ_CONTROL; i < BAD_COPY_COUNT; ++i)
        CHECK(expects_abort((enum BadCopy)i));
    puts("PASS 10 host-copy alias/overflow denials, normal copies and zero-byte no-ops");
#else
    puts("SKIP: negative fork oracle requires POSIX; positive copies passed");
#endif
    return 0;
}
