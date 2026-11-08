#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* This harness is linked against an ACTUAL source-compiled .atto LLVM module.
 * Only its native process main wrapper is renamed by the test driver to avoid
 * two main definitions. The actor init and handler are the untouched
 * compiler-generated symbols and call the M3 interpreter through native
 * actor-incarnation frame bytes. No host imitation of guest function code.
 */
extern void oreslang_actor_Worker_init(
    OresActorI64Heap*, uint64_t, uint64_t);
extern int64_t oreslang_actor_Worker_on_message(
    OresActorI64Heap*, uint64_t, uint64_t, int64_t);

static void check(int condition) { if (!condition) abort(); }

int main(int argc, char** argv) {
    check(argc == 2);
    errno = 0;
    char* end = NULL;
    const long long input = strtoll(argv[1], &end, 10);
    check(errno == 0 && end && end != argv[1] && *end == 0);
    check(input >= 0 && input <= INT_MAX);

    void* actor = oreslang_source_actor_spawn(
        oreslang_actor_Worker_init,
        oreslang_actor_Worker_on_message, 1);
    check(actor != NULL);
    oreslang_source_actor_send(actor, (int64_t)input);

    struct timespec start, now;
    check(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (oreslang_source_actor_test_processed(actor) != 1) {
        check(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        check(now.tv_sec - start.tv_sec < 20);
        sched_yield();
    }
    /* If a frame quota was reached, the runtime automatically sealed the
     * actor after the completed callback; user-owned stop/join is still
     * permitted once and reclaims its private frame+heap allocations. */
    oreslang_source_actor_stop(actor);
    puts("ACTUAL_SOURCE_COMPILED_ACTOR_FRAME_VM_EXECUTED processed=1");
    return 0;
}
