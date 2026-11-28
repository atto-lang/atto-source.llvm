#ifndef ATTO_HOST_ENV_SNAPSHOT_V1_H
#define ATTO_HOST_ENV_SNAPSHOT_V1_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host-only staging for atto.std/env; not compiler lowering or authorization.
 * Trusted embedder supplies a per-isolate snapshot and authenticated coarse
 * ENV grant. No getenv/environ reads; no guest-side system calls. */
typedef struct atto_host_env_snapshot_v1 atto_host_env_snapshot_v1;
typedef struct {
    const char *name;             /* ASCII identifier, at most 128 bytes */
    const unsigned char *value;   /* explicitly supplied host bytes */
    size_t value_length;          /* 0..16384 validated UTF-8 bytes */
} atto_host_env_entry_v1;
typedef enum {
    ATTO_ENV_V1_OK = 0,
    ATTO_ENV_V1_ABSENT = 1,
    ATTO_ENV_V1_DENIED = 2,
    ATTO_ENV_V1_INVALID = 3,
    ATTO_ENV_V1_TOO_SMALL = 4,
    ATTO_ENV_V1_NO_MEMORY = 5
} atto_host_env_status_v1;

/* Allowed/denied rules: "*" | "KEY" | "PREFIX*"; denies win.
 * Source entry values are copied only after both coarse and scoped grants.
 * Initialization may allocate; Get/Has do not allocate. */
atto_host_env_status_v1 atto_host_env_create_v1(
    const atto_host_env_entry_v1 *entries, size_t entry_count,
    const char *const *allows, size_t allow_count,
    const char *const *denies, size_t deny_count,
    bool coarse_env_capability, atto_host_env_snapshot_v1 **out);

/* Caller supplies output buffer. On OK, received is exact byte count.
 * Absent granted key is ABSENT; denied/invalid errors do not reveal values. */
atto_host_env_status_v1 atto_host_env_get_v1(
    const atto_host_env_snapshot_v1 *env, const char *name,
    unsigned char *output, size_t output_capacity, size_t *received);
atto_host_env_status_v1 atto_host_env_has_v1(
    const atto_host_env_snapshot_v1 *env, const char *name, bool *present);
/* Caller must quiesce all readers before revoke/destroy; not concurrent-safe. */
bool atto_host_env_revoke_v1(atto_host_env_snapshot_v1 *env);
void atto_host_env_destroy_v1(atto_host_env_snapshot_v1 *env);
#ifdef __cplusplus
}
#endif
#endif
