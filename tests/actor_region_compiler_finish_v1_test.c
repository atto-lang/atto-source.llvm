/* Native substrate regression for LLVM compiler-generated lexical cleanup.
 * Compiled .atto evidence is in actor_region_v2_multiowner_guest.cmake.
 * This source-free host test MUST NOT be represented as guest execution. */
#include "oreslang/runtime/ActorRegionV1.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "actor compiler finish failure at line %d: %s\n", __LINE__, #cond); \
    return 1; \
} } while (0)

static int success_batch(unsigned count, uint64_t incarnation) {
    OresActorRegionV1* arena = ores_actor_region_v1_open(77, incarnation, count, 64, 64);
    OresArenaTurn turn;
    OresActorRegionV1Ref refs[ORES_ACTOR_ARENA_MAX_SCOPED_OWNERS];
    OresActorRegionV1Ref* reverse[ORES_ACTOR_ARENA_MAX_SCOPED_OWNERS];
    ores_actor_region_v1_compiler_enter(arena, 77, incarnation, &turn);
    for (unsigned i = 0; i < count; ++i) {
        ores_actor_region_v1_compiler_alloc(arena, &turn, 8, &refs[i]);
        ores_actor_region_v1_compiler_write_i64(arena, &turn, &refs[i], 0, 80 + i);
        CHECK(ores_actor_region_v1_compiler_read_i64(
                  arena, &turn, &refs[i], 0) == (int64_t)(80 + i));
        reverse[i] = &refs[count - i - 1];
    }
    /* Even incarnations simulate a supervisor sealing admission while the
     * already admitted native owner turn is still active. New reads/writes
     * must stop, but its compiler-inserted destructor path must finish. */
    if ((incarnation & 1u) == 0)
        ores_actor_region_v1_retire(arena);
    /* The original pointers retain domain/generation evidence until the
     * runtime has authenticated all N distinct owners in this active turn. */
    ores_actor_region_v1_compiler_finish(arena, &turn, reverse, count);
    for (unsigned i = 0; i < count; ++i)
        CHECK(refs[i].offset.storage == 0 && refs[i].proof.domain == 0 &&
              refs[i].proof.generation == 0);
    /* arena has been zeroized and physically freed by compiler_finish. */
    return 0;
}

#if defined(__unix__) || defined(__APPLE__)
enum BadFinish {
    DUPLICATE_OWNER, EXTRA_UNLISTED_OWNER, BORROWED_LAST_OWNER,
    STALE_LATER_GENERATION, FORGED_LATER_OFFSET, FOREIGN_LATER_DOMAIN,
    MISSING_COUNT, EXCESSIVE_COUNT, NULL_OWNER_POINTER,
    STALE_TURN, ALREADY_DROPPED_OWNER, READ_AFTER_RETIRE,
    WRITE_AFTER_RETIRE, ALLOC_AFTER_RETIRE
};

static void attempt_bad(enum BadFinish scenario) {
    OresActorRegionV1* arena = ores_actor_region_v1_open(91, 1, 2, 64, 64);
    OresArenaTurn turn;
    OresActorRegionV1Ref refs[2];
    OresActorRegionV1Ref* order[2] = {&refs[1], &refs[0]};
    ores_actor_region_v1_compiler_enter(arena, 91, 1, &turn);
    ores_actor_region_v1_compiler_alloc(arena, &turn, 8, &refs[0]);
    ores_actor_region_v1_compiler_alloc(arena, &turn, 8, &refs[1]);
    switch (scenario) {
        case DUPLICATE_OWNER:
            order[1] = &refs[1];
            break;
        case EXTRA_UNLISTED_OWNER:
            ores_actor_region_v1_compiler_finish(arena, &turn, order, 1);
            return;
        case BORROWED_LAST_OWNER:
            (void)ores_actor_region_v1_borrow(arena, turn, refs[0]);
            break;
        case STALE_LATER_GENERATION:
            ++refs[0].proof.generation;
            break;
        case FORGED_LATER_OFFSET:
            ++refs[0].offset.storage;
            break;
        case FOREIGN_LATER_DOMAIN:
            ++refs[0].proof.domain;
            break;
        case MISSING_COUNT:
            ores_actor_region_v1_compiler_finish(arena, &turn, order, 0);
            return;
        case EXCESSIVE_COUNT:
            ores_actor_region_v1_compiler_finish(arena, &turn, order, 9);
            return;
        case NULL_OWNER_POINTER:
            order[1] = NULL;
            break;
        case STALE_TURN:
            ++turn.epoch;
            break;
        case ALREADY_DROPPED_OWNER:
            ores_actor_region_v1_compiler_drop(arena, &turn, &refs[0]);
            break;
        case READ_AFTER_RETIRE:
            ores_actor_region_v1_retire(arena);
            (void)ores_actor_region_v1_compiler_read_i64(
                arena, &turn, &refs[0], 0);
            return;
        case WRITE_AFTER_RETIRE:
            ores_actor_region_v1_retire(arena);
            ores_actor_region_v1_compiler_write_i64(
                arena, &turn, &refs[0], 0, 3);
            return;
        case ALLOC_AFTER_RETIRE:
            ores_actor_region_v1_retire(arena);
            ores_actor_region_v1_compiler_alloc(
                arena, &turn, 8, &refs[0]);
            return;
    }
    ores_actor_region_v1_compiler_finish(arena, &turn, order, 2);
}

static int must_abort(enum BadFinish scenario) {
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        const struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        attempt_bad(scenario);
        _exit(93); /* silent acceptance is a test failure */
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
           WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    CHECK(sizeof(OresActorRegionV1Ref) == 32);
    for (uint64_t incarnation = 1; incarnation <= 100; ++incarnation) {
        for (unsigned count = 1; count <= ORES_ACTOR_ARENA_MAX_SCOPED_OWNERS; ++count)
            CHECK(success_batch(count, incarnation) == 0);
    }
#if defined(__unix__) || defined(__APPLE__)
    for (int scenario = DUPLICATE_OWNER; scenario <= ALLOC_AFTER_RETIRE; ++scenario)
        CHECK(must_abort((enum BadFinish)scenario));
#endif
    puts("ACTOR_REGION_COMPILER_ATOMIC_FINISH_HOST_PASS");
    return 0;
}
