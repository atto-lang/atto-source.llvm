#define _POSIX_C_SOURCE 200809L
#include "atto/actor_process_isolation.h"

#include <errno.h>
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

    /* Do not leak API keys or caller-specific execution environment into a
     * crash-isolated worker. A production policy must also close unapproved
     * inherited descriptors and apply OS sandbox / cgroup restrictions. */
    char *const safe_env[] = {"PATH=/usr/bin:/bin", "LC_ALL=C", NULL};
    pid_t pid = -1;
    const int err = posix_spawn(&pid, spec->executable, NULL, NULL,
                                (char *const *)spec->argv, safe_env);
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
