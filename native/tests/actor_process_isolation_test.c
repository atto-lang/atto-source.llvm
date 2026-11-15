#define _POSIX_C_SOURCE 200809L
#include "atto/actor_process_isolation.h"
/* Release builds define NDEBUG, disabling assert() and accidentally skipping
 * test operations that were written inside assertions. Keep all executable
 * fault-containment checks active in both Debug and Release tests. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static atto_actor_exit_notice run(const char *exe, const char *mode, int timeout_ms) {
    const char *argv[] = {exe, mode, NULL};
    const atto_actor_worker_spec spec = {17u, 3u, 44u, exe, argv, (uint32_t)timeout_ms};
    atto_actor_exit_notice n;
    assert(atto_actor_run_isolated(&spec, &n) == 0);
    assert(n.actor_id == 17u && n.supervisor_id == 3u && n.incarnation == 44u);
    return n;
}

typedef struct sibling_test {
    const char *executable;
    atto_actor_exit_notice result;
} sibling_test;

static void *run_healthy_sibling(void *opaque) {
    sibling_test *s = (sibling_test *)opaque;
    s->result = run(s->executable, "slow-clean", 3000);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc == 2) {
        const struct rlimit no_core = {0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        if (!strcmp(argv[1], "clean")) return 0;
        if (!strcmp(argv[1], "code17")) return 17;
        if (!strcmp(argv[1], "segv")) { raise(SIGSEGV); return 90; }
        if (!strcmp(argv[1], "abort")) { abort(); return 90; }
        if (!strcmp(argv[1], "bus")) { raise(SIGBUS); return 90; }
        if (!strcmp(argv[1], "illegal")) { raise(SIGILL); return 90; }
        if (!strcmp(argv[1], "floating")) { raise(SIGFPE); return 90; }
        if (!strcmp(argv[1], "kill")) { raise(SIGKILL); return 90; }
        if (!strcmp(argv[1], "slow-clean")) {
            struct timespec wait = {0, 160000000L};
            (void)nanosleep(&wait, NULL);
            return 0;
        }
        if (!strcmp(argv[1], "hang")) {
            struct timespec wait = {10, 0};
            (void)nanosleep(&wait, NULL);
            return 90;
        }
        if (!strcmp(argv[1], "environment")) return getenv("TEST_SECRET_MUST_NOT_LEAK") ? 99 : 0;
        return 91;
    }
    assert(argc >= 1 && argv[0][0] == '/');
    const char *exe = argv[0];
    atto_actor_exit_notice n = run(exe, "segv", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGSEGV);
    /* The process supervising this worker is still alive and can launch
     * another actor after a fatal child SIGSEGV. */
    sibling_test sibling = { .executable = exe };
    pthread_t sibling_thread;
    assert(pthread_create(&sibling_thread, NULL, run_healthy_sibling, &sibling) == 0);
    /* The healthy sibling remains alive while an independent actor crashes. */
    n = run(exe, "segv", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGSEGV);
    assert(pthread_join(sibling_thread, NULL) == 0);
    assert(sibling.result.kind == ATTO_ACTOR_EXIT_CLEAN);
    n = run(exe, "clean", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_CLEAN && n.exit_code == 0);
    n = run(exe, "code17", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_CODE && n.exit_code == 17);
    n = run(exe, "abort", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGABRT);
    n = run(exe, "bus", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGBUS);
    n = run(exe, "illegal", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGILL);
    n = run(exe, "floating", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGFPE);
    n = run(exe, "kill", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGKILL);
    n = run(exe, "hang", 60);
    assert(n.kind == ATTO_ACTOR_EXIT_TIMEOUT);
    assert(setenv("TEST_SECRET_MUST_NOT_LEAK", "private", 1) == 0);
    n = run(exe, "environment", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_CLEAN);
    assert(unsetenv("TEST_SECRET_MUST_NOT_LEAK") == 0);
    n = run("/definitely/no/such/atto-worker", "clean", 100);
    assert(n.kind == ATTO_ACTOR_EXIT_SPAWN_ERROR && n.os_error != 0);
    const char *a[] = {exe, "clean", NULL};
    const atto_actor_worker_spec invalid = {17u,3u,44u,exe,a,0u};
    assert(atto_actor_run_isolated(&invalid, &n) == -1);
    assert(n.kind == ATTO_ACTOR_EXIT_SPAWN_ERROR && n.os_error == EINVAL);
    puts("ATTO_PROCESS_ISOLATED_ACTOR_FAULT_TEST_PASS");
    return 0;
}
