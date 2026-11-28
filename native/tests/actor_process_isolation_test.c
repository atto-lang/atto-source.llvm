#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _DEFAULT_SOURCE 1
#endif
#include "atto/actor_process_isolation.h"
/* Release builds define NDEBUG, disabling assert() and accidentally skipping
 * test operations that were written inside assertions. Keep all executable
 * fault-containment checks active in both Debug and Release tests. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
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

/* Darwin can report a protection fault as SIGBUS (EXC_BAD_ACCESS).
 * Keep Linux strict and separately test explicitly raised signal identities. */
static int is_protected_page_fault(atto_actor_exit_notice n) {
    if (n.kind != ATTO_ACTOR_EXIT_SIGNAL) return 0;
#if defined(__APPLE__)
    return n.signal_number == SIGSEGV || n.signal_number == SIGBUS;
#else
    return n.signal_number == SIGSEGV;
#endif
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
        if (!strcmp(argv[1], "pagefault")) {
            /* Real inaccessible memory, independent of /dev/zero support.
             * Use the host page size (including Darwin arm64) and prove the
             * mapping is writable before revoking all access. */
            const long page_size = sysconf(_SC_PAGESIZE);
            if (page_size <= 0) return 92;
            void *page = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (page == MAP_FAILED) return 93;
            *(volatile unsigned char *)page = 42;
            if (mprotect(page, (size_t)page_size, PROT_NONE) != 0) {
                (void)munmap(page, (size_t)page_size);
                return 95;
            }
            volatile unsigned char observed = *(volatile unsigned char *)page;
            (void)observed;
            return 94;
        }
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
        if (!strcmp(argv[1], "fd-closed")) {
            errno = 0;
            return fcntl(123, F_GETFD) == -1 && errno == EBADF ? 0 : 88;
        }
        if (!strcmp(argv[1], "stdio-null")) {
            struct stat devnull, actual;
            if (stat("/dev/null", &devnull) != 0) return 85;
            for (int fd = 0; fd <= 2; ++fd) {
                if (fstat(fd, &actual) != 0 ||
                    !S_ISCHR(actual.st_mode) ||
                    actual.st_rdev != devnull.st_rdev) return 86;
            }
            return 0;
        }
        if (!strcmp(argv[1], "term")) { raise(SIGTERM); return 87; }
        if (!strcmp(argv[1], "pipe")) { raise(SIGPIPE); return 87; }
        return 91;
    }
    assert(argc >= 1 && argv[0][0] == '/');
    const char *exe = argv[0];
    atto_actor_exit_notice n = run(exe, "pagefault", 5000);
    if (!is_protected_page_fault(n)) {
        fprintf(stderr, "protected-page worker: kind=%d exit=%d signal=%d os_error=%d (%s)\n",
                (int)n.kind, n.exit_code, n.signal_number, n.os_error,
                strerror(n.os_error));
    }
    assert(is_protected_page_fault(n));
    /* The process supervising this worker is still alive and can launch
     * another actor after a fatal child protection fault. */
    sibling_test sibling = { .executable = exe };
    pthread_t sibling_thread;
    assert(pthread_create(&sibling_thread, NULL, run_healthy_sibling, &sibling) == 0);
    /* The healthy sibling remains alive while an independent actor crashes. */
    n = run(exe, "pagefault", 5000);
    assert(is_protected_page_fault(n));
    assert(pthread_join(sibling_thread, NULL) == 0);
    assert(sibling.result.kind == ATTO_ACTOR_EXIT_CLEAN);
    n = run(exe, "segv", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGSEGV);
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

    /* An actual open non-CLOEXEC descriptor in the parent must never reach
     * the isolated child, even if the parent is multi-threaded. */
    int fd = open("/dev/null", O_RDONLY);
    assert(fd >= 0);
    assert(dup2(fd, 123) == 123);
    if (fd != 123) assert(close(fd) == 0);
    n = run(exe, "fd-closed", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_CLEAN);
    assert(close(123) == 0);
    n = run(exe, "stdio-null", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_CLEAN);

    /* Child signal state must be independent of the supervisor's state. */
    sigset_t block, old_mask;
    assert(sigemptyset(&block) == 0);
    assert(sigaddset(&block, SIGTERM) == 0);
    assert(pthread_sigmask(SIG_BLOCK, &block, &old_mask) == 0);
    n = run(exe, "term", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGTERM);
    assert(pthread_sigmask(SIG_SETMASK, &old_mask, NULL) == 0);

    struct sigaction ignored = {0}, old_action;
    ignored.sa_handler = SIG_IGN;
    assert(sigemptyset(&ignored.sa_mask) == 0);
    assert(sigaction(SIGPIPE, &ignored, &old_action) == 0);
    n = run(exe, "pipe", 5000);
    assert(n.kind == ATTO_ACTOR_EXIT_SIGNAL && n.signal_number == SIGPIPE);
    assert(sigaction(SIGPIPE, &old_action, NULL) == 0);
    n = run("/definitely/no/such/atto-worker", "clean", 100);
    assert(n.kind == ATTO_ACTOR_EXIT_SPAWN_ERROR && n.os_error != 0);
    const char *a[] = {exe, "clean", NULL};
    const atto_actor_worker_spec invalid = {17u,3u,44u,exe,a,0u};
    assert(atto_actor_run_isolated(&invalid, &n) == -1);
    assert(n.kind == ATTO_ACTOR_EXIT_SPAWN_ERROR && n.os_error == EINVAL);
    puts("ATTO_PROCESS_ISOLATED_ACTOR_FAULT_TEST_PASS");
    return 0;
}
