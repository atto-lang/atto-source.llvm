#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/SourceActorBootstrap.h"
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

static void invoke_bad(void* bad_ref, int operation) {
    if (operation == 0) oreslang_source_actor_send(bad_ref, 42);
    else if (operation == 1) oreslang_source_actor_stop(bad_ref);
    else (void)oreslang_source_actor_await_ready(bad_ref, resume, 1);
}

/* A fresh exec isolates each actor runtime. Never fork and then run
 * pthread-based actor code inside an inherited multithreaded process. */
static void expect_abort(const char* self, int phase, int operation) {
    pid_t child = fork();
    check(child >= 0);
    if (child == 0) {
        char phase_buf[16], action_buf[16];
        (void)snprintf(phase_buf, sizeof(phase_buf), "%d", phase);
        (void)snprintf(action_buf, sizeof(action_buf), "%d", operation);
        char* const args[] = {(char*)self, phase_buf, action_buf, NULL};
        execvp(self, args);
        _exit(127);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child);
    check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(int argc, char** argv) {
    if (argc == 3) {
        struct rlimit limit = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &limit);
        const int phase = atoi(argv[1]);
        const int action = atoi(argv[2]);
        check(phase >= 0 && phase <= 2 && action >= 0 && action <= 2);
        void* target = (void*)(uintptr_t)1;
        if (phase != 0) {
            target = oreslang_source_actor_spawn(init, message, 1);
            check(target != NULL);
            if (phase == 1) {
                /* A different root keeps the singleton VM allocated, but
                 * retirement removes target from the live membership set. */
                void* survivor = oreslang_source_actor_spawn(init, message, 1);
                check(survivor != NULL);
            }
            oreslang_source_actor_stop(target);
        }
        invoke_bad(target, action);
        return 0;  /* any return is a failed expected rejection */
    }
    check(argc == 1);
    for (int phase = 0; phase < 3; ++phase)
        for (int action = 0; action < 3; ++action)
            expect_abort(argv[0], phase, action);
    puts("SOURCE_ACTOR_OPAQUE_REF_VALIDATION_PASS rejection_scenarios=9");
    return 0;
}
