#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void check(int yes) { if (!yes) abort(); }

static void init(OresActorI64Heap* heap, uint64_t domain, uint64_t epoch) {
    (void)heap;
    check(domain != 0 && epoch != 0);
}
static int64_t message(OresActorI64Heap* heap, uint64_t domain,
                       uint64_t epoch, int64_t data) {
    (void)heap;
    check(domain != 0 && epoch != 0);
    return data;
}
static int64_t resume(void* actor) { (void)actor; return 1; }

/* An invalid ActorRef is a contract rejection (SIGABRT), not an
 * out-of-bounds dereference (SIGSEGV) or use-after-free. Fork only from
 * the trusted test process with no concurrent actor carrier callbacks. */
static void expect_abort(void* bad_ref, int operation) {
    pid_t child = fork();
    check(child >= 0);
    if (child == 0) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        if (operation == 0) oreslang_source_actor_send(bad_ref, 42);
        else if (operation == 1) oreslang_source_actor_stop(bad_ref);
        else (void)oreslang_source_actor_await_ready(bad_ref, resume, 1);
        _exit(0);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(void) {
    void* forged = (void*)(uintptr_t)1;
    for (int action = 0; action < 3; ++action) expect_abort(forged, action);

    /* Keep a second root actor alive while testing stale pointers against
     * a still-open VM; membership must reject without freed-object reads. */
    void* retired = oreslang_source_actor_spawn(init, message, 1);
    void* survivor = oreslang_source_actor_spawn(init, message, 1);
    check(retired != NULL && survivor != NULL);
    oreslang_source_actor_stop(retired);
    for (int action = 0; action < 3; ++action) expect_abort(retired, action);
    oreslang_source_actor_stop(survivor);
    for (int action = 0; action < 3; ++action) expect_abort(retired, action);
    puts("SOURCE_ACTOR_OPAQUE_REF_VALIDATION_PASS rejection_scenarios=9");
    return 0;
}
