#ifndef ATTO_ACTOR_PROCESS_ISOLATION_H
#define ATTO_ACTOR_PROCESS_ISOLATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Process-isolation proof substrate; NOT an in-process SIGSEGV recovery API.
 * Returned notice is an observable exit, not an assertion of guest-language
 * actor supervision, sandbox confinement, or successful resource reclamation. */
typedef enum atto_actor_exit_kind {
    ATTO_ACTOR_EXIT_CLEAN = 1,
    ATTO_ACTOR_EXIT_CODE = 2,
    ATTO_ACTOR_EXIT_SIGNAL = 3,
    ATTO_ACTOR_EXIT_TIMEOUT = 4,
    ATTO_ACTOR_EXIT_SPAWN_ERROR = 5,
    ATTO_ACTOR_EXIT_WAIT_ERROR = 6
} atto_actor_exit_kind;

typedef struct atto_actor_worker_spec {
    uint64_t actor_id;
    uint64_t supervisor_id;
    uint64_t incarnation;
    const char *executable;             /* mandatory absolute trusted executable */
    const char *const *argv;             /* NULL-terminated, argv[0] required */
    uint32_t timeout_ms;                /* 1 .. 60000 inclusive */
} atto_actor_worker_spec;

typedef struct atto_actor_exit_notice {
    uint64_t actor_id;
    uint64_t supervisor_id;
    uint64_t incarnation;
    atto_actor_exit_kind kind;
    int exit_code;                      /* only valid for EXIT_CODE / CLEAN */
    int signal_number;                  /* only valid for EXIT_SIGNAL */
    int os_error;                       /* spawn/wait errno or validation EINVAL */
} atto_actor_exit_notice;

/* 0: populated notice, including failed spawn/wait. -1: invalid pointers or
 * identifiers/time budget; notice is populated when non-NULL. Does not use a
 * shell or inherit the parent's environment variables. Waits for ONE pid.
 * No actor arena is freed by this function; retirement must be controlled by
 * the caller and guarded by independently verified worker quiescence. */
int atto_actor_run_isolated(const atto_actor_worker_spec *spec,
                            atto_actor_exit_notice *notice);

#ifdef __cplusplus
}
#endif
#endif
