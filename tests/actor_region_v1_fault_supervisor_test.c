/*
 * HOST-ONLY process fault-containment regression for ActorRegionV1.
 *
 * Deliberately execs a fresh process rather than calling untrusted code in
 * the supervisor: ActorArena currently aborts on misuse and setjmp, signal
 * handlers, or C++ exception catching cannot safely recover from an abort.
 * This test does NOT implement the production RPC/sandbox/capability gate,
 * nor does it exercise compiler-generated .atto code.
 */
#include "oreslang/runtime/ActorRegionV1.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "actor-region supervisor test failed at %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static int worker(const char *mode) {
    const uint64_t incarnation = strcmp(mode, "restart") == 0 ? 2u : 1u;
    OresActorRegionV1 *a = ores_actor_region_v1_open(501, incarnation, 1, 32, 32);
    if (strcmp(mode, "wrong-incarnation") == 0) {
        (void)ores_actor_region_v1_enter(a, 501, incarnation + 1u);
        return 99;
    }
    OresArenaTurn turn = ores_actor_region_v1_enter(a, 501, incarnation);
    OresActorRegionV1Ref ref = ores_actor_region_v1_alloc(a, turn, 8);
    uint64_t value = UINT64_C(0x1122334455667788);
    ores_actor_region_v1_write(a, turn, ref, 0, &value, sizeof(value));

    if (strcmp(mode, "forged-generation") == 0) {
        ++ref.proof.generation;
        ores_actor_region_v1_read(a, turn, ref, 0, &value, sizeof(value));
        return 98;
    }
    if (strcmp(mode, "foreign-actor") == 0) {
        /* Access on the foreign region is made on its own authenticated
         * carrier turn; proof-domain mismatch must still abort. */
        ores_actor_region_v1_leave(a, turn);
        OresActorRegionV1 *b = ores_actor_region_v1_open(502, 1, 1, 32, 32);
        OresArenaTurn foreign_turn = ores_actor_region_v1_enter(b, 502, 1);
        ores_actor_region_v1_read(b, foreign_turn, ref, 0, &value, sizeof(value));
        return 97;
    }
    if (strcmp(mode, "stale-slot") == 0 ||
        strcmp(mode, "double-drop") == 0) {
        OresActorRegionV1Ref old = ref;
        ores_actor_region_v1_drop(a, turn, &ref);
        if (strcmp(mode, "stale-slot") == 0) {
            OresActorRegionV1Ref next = ores_actor_region_v1_alloc(a, turn, 8);
            if (next.offset.storage != old.offset.storage ||
                next.proof.generation == old.proof.generation) return 96;
        }
        ores_actor_region_v1_drop(a, turn, &old);
        return 95;
    }
    if (strcmp(mode, "exhaustion") == 0) {
        (void)ores_actor_region_v1_alloc(a, turn, 8);
        return 94;
    }
    if (strcmp(mode, "retired-access") == 0) {
        ores_actor_region_v1_retire(a);
        ores_actor_region_v1_read(a, turn, ref, 0, &value, sizeof(value));
        return 93;
    }
    if (strcmp(mode, "normal") != 0 && strcmp(mode, "restart") != 0)
        return 92;

    uint64_t actual = 0;
    ores_actor_region_v1_read(a, turn, ref, 0, &actual, sizeof(actual));
    if (actual != value || ref.offset.storage != 1u) return 91;
    ores_actor_region_v1_drop(a, turn, &ref);
    if (ref.offset.storage != 0u) return 90;
    ores_actor_region_v1_leave(a, turn);
    ores_actor_region_v1_retire(a);
    if (!ores_actor_region_v1_try_destroy(a)) return 89;
    return 0;
}

#if defined(__unix__) || defined(__APPLE__)
static int isolated_worker(const char *executable, const char *mode,
                           int expect_abort) {
    pid_t pid = (pid_t)-1;
    char *const argv[] = {(char *)executable, (char *)mode, NULL};
    const int spawn_status = posix_spawn(&pid, executable, NULL, NULL, argv, environ);
    if (spawn_status != 0 || pid <= 0) return 0;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return 0;
    if (expect_abort) return WIFSIGNALED(status) &&
        WTERMSIG(status) == SIGABRT;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

int main(int argc, char **argv) {
    if (argc == 2) {
#if defined(__unix__) || defined(__APPLE__)
        struct rlimit no_cores = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_cores);
#endif
        return worker(argv[1]);
    }
    CHECK(argc == 1);
#if defined(__unix__) || defined(__APPLE__)
    CHECK(isolated_worker(argv[0], "normal", 0));
    CHECK(isolated_worker(argv[0], "forged-generation", 1));
    CHECK(isolated_worker(argv[0], "foreign-actor", 1));
    CHECK(isolated_worker(argv[0], "stale-slot", 1));
    CHECK(isolated_worker(argv[0], "double-drop", 1));
    CHECK(isolated_worker(argv[0], "exhaustion", 1));
    CHECK(isolated_worker(argv[0], "retired-access", 1));
    CHECK(isolated_worker(argv[0], "wrong-incarnation", 1));
    /* A fresh, independently spawned worker remains executable after the
     * earlier guest failures; no stale proof is transported over IPC. */
    CHECK(isolated_worker(argv[0], "restart", 0));
    puts("ACTOR_REGION_V1_HOST_ONLY|child_faults=7|owner_fence=1|restart=1|supervisor_alive=1|PASS");
#else
    /* Native POSIX process isolation is not a Windows ABI implementation. */
    puts("ACTOR_REGION_V1_HOST_ONLY|SKIP=no-posix-process-boundary");
#endif
    return 0;
}
