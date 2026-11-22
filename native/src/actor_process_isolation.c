#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "atto/actor_process_isolation.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
#define ATTO_SPAWN_DEFAULT_CLOSE_SUPPORTED 1
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
#define ATTO_SPAWN_DEFAULT_CLOSE_SUPPORTED 1
#endif
#endif

/* This interface intentionally accepts only absolute trusted executable
 * paths. Unlike fork()+guest-code in a multithreaded runtime, posix_spawn()
 * executes another program; the parent never executes guest code. */
static uint64_t elapsed_millis(const struct timespec *start,
                               const struct timespec *now) {
    const uint64_t sec = (uint64_t)(now->tv_sec - start->tv_sec);
    const long nsec = now->tv_nsec - start->tv_nsec;
    if (nsec >= 0) return sec * 1000u + (uint64_t)nsec / 1000000u;
    return (sec - 1u) * 1000u + (uint64_t)(nsec + 1000000000L) / 1000000u;
}

static int reap_blocking(pid_t pid, int *status) {
    for (;;) {
        const pid_t got = waitpid(pid, status, 0);
        if (got == pid) return 0;
        if (got < 0 && errno == EINTR) continue;
        return -1;
    }
}

/* An explicit, fail-closed descriptor/signal policy is required before a
 * worker is launched. Merely passing a sanitized envp does NOT prevent
 * inheriting the supervisor's open sockets, credentials, and pipes.
 *
 * On GNU libc >= 2.34, closefrom runs as a spawn file action (without an
 * racy scan of the parent's descriptors). On Apple, CLOEXEC_DEFAULT supplies
 * equivalent default-close behavior. Unsupported targets return ENOTSUP.
 * Neither mechanism constitutes a complete OS security sandbox. */
static int spawn_worker(pid_t *pid, const atto_actor_worker_spec *spec) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) return rc;
    rc = posix_spawnattr_init(&attrs);
    if (rc != 0) {
        (void)posix_spawn_file_actions_destroy(&actions);
        return rc;
    }

#if defined(__APPLE__)
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    const short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                        POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
    rc = ENOTSUP;
    goto cleanup;
#endif
#elif defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 34)
    const short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    rc = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    if (rc != 0) goto cleanup;
#else
    rc = ENOTSUP;
    goto cleanup;
#endif
#else
    rc = ENOTSUP;
    goto cleanup;
#endif

#if defined(ATTO_SPAWN_DEFAULT_CLOSE_SUPPORTED)
    /* Workers may not inherit the supervisor's stdin/out/err. Real IPC must
     * be an explicit, authenticated allowlisted endpoint in a later API. */
    rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,
                                           "/dev/null", O_RDONLY, 0);
    if (rc != 0) goto cleanup;
    rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                           "/dev/null", O_WRONLY, 0);
    if (rc != 0) goto cleanup;
    rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                           "/dev/null", O_WRONLY, 0);
    if (rc != 0) goto cleanup;

    /* Clear masks and restore fatal/termination defaults: an ignored SIGPIPE
     * or blocked SIGTERM in the parent must not change child semantics. */
    sigset_t empty_mask, default_signals;
    if (sigemptyset(&empty_mask) != 0 ||
        sigemptyset(&default_signals) != 0) {
        rc = errno;
        goto cleanup;
    }
    const int resets[] = {
        SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTERM,
        SIGINT, SIGQUIT, SIGPIPE, SIGALRM, SIGUSR1, SIGUSR2
    };
    for (size_t i = 0; i < sizeof(resets) / sizeof(resets[0]); ++i) {
        if (sigaddset(&default_signals, resets[i]) != 0) {
            rc = errno;
            goto cleanup;
        }
    }
    rc = posix_spawnattr_setsigmask(&attrs, &empty_mask);
    if (rc != 0) goto cleanup;
    rc = posix_spawnattr_setsigdefault(&attrs, &default_signals);
    if (rc != 0) goto cleanup;
    rc = posix_spawnattr_setflags(&attrs, flags);
    if (rc != 0) goto cleanup;

    char *const safe_env[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", NULL};
    rc = posix_spawn(pid, spec->executable, &actions, &attrs,
                     (char *const *)spec->argv, safe_env);
#endif

cleanup:
    (void)posix_spawnattr_destroy(&attrs);
    (void)posix_spawn_file_actions_destroy(&actions);
    return rc;
}

int atto_actor_run_isolated(const atto_actor_worker_spec *spec,
                            atto_actor_exit_notice *notice) {
    if (!notice) return -1;
    memset(notice, 0, sizeof(*notice));
    notice->kind = ATTO_ACTOR_EXIT_SPAWN_ERROR;
    notice->os_error = EINVAL;
    if (!spec) return -1;
    notice->actor_id = spec->actor_id;
    notice->supervisor_id = spec->supervisor_id;
    notice->incarnation = spec->incarnation;
    if (!spec->actor_id || !spec->supervisor_id || !spec->incarnation ||
            !spec->executable || spec->executable[0] != '/' ||
            !spec->argv || !spec->argv[0] ||
            spec->timeout_ms == 0 || spec->timeout_ms > 60000) return -1;

    /* The launch helper rejects platforms without atomic default-close. */
    pid_t pid = -1;
    const int err = spawn_worker(&pid, spec);
    if (err) {
        notice->os_error = err;
        return 0;
    }
    notice->os_error = 0;
    struct timespec start = {0, 0}, now = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        notice->kind = ATTO_ACTOR_EXIT_WAIT_ERROR;
        notice->os_error = errno;
        (void)kill(pid, SIGKILL);
        (void)reap_blocking(pid, &(int){0});
        return 0;
    }

    for (;;) {
        int status = 0;
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            if (WIFEXITED(status)) {
                notice->exit_code = WEXITSTATUS(status);
                notice->kind = notice->exit_code == 0 ? ATTO_ACTOR_EXIT_CLEAN : ATTO_ACTOR_EXIT_CODE;
            } else if (WIFSIGNALED(status)) {
                notice->kind = ATTO_ACTOR_EXIT_SIGNAL;
                notice->signal_number = WTERMSIG(status);
            } else {
                notice->kind = ATTO_ACTOR_EXIT_WAIT_ERROR;
                notice->os_error = ECHILD;
            }
            return 0;
        }
        if (got < 0) {
            if (errno == EINTR) continue;
            notice->kind = ATTO_ACTOR_EXIT_WAIT_ERROR;
            notice->os_error = errno;
            return 0;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            notice->kind = ATTO_ACTOR_EXIT_WAIT_ERROR;
            notice->os_error = errno;
            (void)kill(pid, SIGKILL);
            (void)reap_blocking(pid, &status);
            return 0;
        }
        if (elapsed_millis(&start, &now) >= spec->timeout_ms) {
            (void)kill(pid, SIGKILL);
            if (reap_blocking(pid, &status) != 0) {
                notice->kind = ATTO_ACTOR_EXIT_WAIT_ERROR;
                notice->os_error = errno;
            } else {
                notice->kind = ATTO_ACTOR_EXIT_TIMEOUT;
            }
            return 0;
        }
        const struct timespec pause = {0, 2000000L};
        (void)nanosleep(&pause, NULL);
    }
}
