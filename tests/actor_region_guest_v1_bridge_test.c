#include "oreslang/runtime/ActorRegionGuestV1.h"
#include <stdint.h>
#include <stdio.h>
#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "ActorRegionGuestV1 bridge failed at %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

static int happy_path(void) {
    ores_actor_region_guest_v1_begin(4, 64, 64);
    const uint32_t first = ores_actor_region_guest_v1_alloc(16);
    const uint32_t second = ores_actor_region_guest_v1_alloc(16);
    CHECK(first == 1u && second == 65u);
    ores_actor_region_guest_v1_write_i64(first, 0, INT64_C(20));
    ores_actor_region_guest_v1_write_i64(first, 8, INT64_C(22));
    ores_actor_region_guest_v1_write_i64(second, 0, INT64_C(9));
    CHECK(ores_actor_region_guest_v1_read_i64(first, 0) == 20);
    CHECK(ores_actor_region_guest_v1_read_i64(first, 8) == 22);
    CHECK(ores_actor_region_guest_v1_read_i64(second, 0) == 9);
    ores_actor_region_guest_v1_drop(second);
    const uint32_t third = ores_actor_region_guest_v1_alloc(16);
    CHECK(third == 129u && third != second);
    ores_actor_region_guest_v1_write_i64(third, 0, INT64_C(11));
    CHECK(ores_actor_region_guest_v1_read_i64(third, 0) == 11);
    ores_actor_region_guest_v1_drop(third);
    ores_actor_region_guest_v1_drop(first);
    ores_actor_region_guest_v1_end();
    return 0;
}

#if defined(__unix__) || defined(__APPLE__)
enum FailureCase { STALE_OFFSET, DOUBLE_DROP, FORGED_OFFSET, LEAK_AT_END, SLOT_REUSE };
static void child_failure(enum FailureCase which) {
    ores_actor_region_guest_v1_begin(2, 64, 64);
    const uint32_t object = ores_actor_region_guest_v1_alloc(16);
    if (which == STALE_OFFSET) {
        ores_actor_region_guest_v1_drop(object);
        (void)ores_actor_region_guest_v1_read_i64(object, 0);
    } else if (which == DOUBLE_DROP) {
        ores_actor_region_guest_v1_drop(object);
        ores_actor_region_guest_v1_drop(object);
    } else if (which == FORGED_OFFSET) {
        (void)ores_actor_region_guest_v1_read_i64(object + 1u, 0);
    } else if (which == SLOT_REUSE) {
        ores_actor_region_guest_v1_drop(object);
        const uint32_t replacement = ores_actor_region_guest_v1_alloc(16);
        if (replacement == object) _exit(89);
        (void)ores_actor_region_guest_v1_read_i64(object, 0);
    } else {
        ores_actor_region_guest_v1_end();
    }
}

static int must_abort(enum FailureCase which) {
    const pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        child_failure(which);
        _exit(90);
    }
    int status = 0;
    return waitpid(pid, &status, 0) == pid &&
        WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}
#endif

int main(void) {
    CHECK(happy_path() == 0);
#if defined(__unix__) || defined(__APPLE__)
    CHECK(must_abort(STALE_OFFSET));
    CHECK(must_abort(DOUBLE_DROP));
    CHECK(must_abort(FORGED_OFFSET));
    CHECK(must_abort(LEAK_AT_END));
    CHECK(must_abort(SLOT_REUSE));
#endif
    puts("ACTOR_REGION_GUEST_V1_HOST_ONLY|sidecar=1|drop=exactly_once|PASS");
    return 0;
}
