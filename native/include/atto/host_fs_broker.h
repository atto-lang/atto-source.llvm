#ifndef ATTO_HOST_FS_BROKER_H
#define ATTO_HOST_FS_BROKER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Linux trusted-host-only policy broker. No guest-visible handles or syscalls.
 * A trusted scheduler must authenticate tenant/generation arguments; these
 * numbers alone are NOT unforgeable credentials. Artifact/host ceilings must
 * be intersected by the caller before constructing the broker. */
#define ATTO_FS_SCOPE_LIMIT 1024u
#define ATTO_FS_MAX_BYTES 1048576u

typedef struct atto_fs_policy {
    const char *allow_read;    /* e.g. "data"; NULL = deny */
    const char *allow_write;   /* e.g. "out"; NULL = deny */
    const char *deny_read;     /* explicit narrow deny; NULL = none */
    const char *deny_write;    /* explicit narrow deny; NULL = none */
    uint64_t tenant_id;       /* trusted identity, not guest-supplied */
    uint64_t generation;      /* trusted actor/TenantVM generation */
} atto_fs_policy;

typedef struct atto_fs_broker {
    int root_fd;
    uint64_t tenant_id;
    uint64_t generation;
    char read_scope[ATTO_FS_SCOPE_LIMIT];
    char write_scope[ATTO_FS_SCOPE_LIMIT];
    char deny_read_scope[ATTO_FS_SCOPE_LIMIT];
    char deny_write_scope[ATTO_FS_SCOPE_LIMIT];
} atto_fs_broker;

/* Return 0 on success, otherwise negative errno; fail closed if openat2
 * unavailable. This intentionally uses a trusted host-supplied root. */
int atto_fs_broker_init(atto_fs_broker *out, const char *trusted_root,
                        const atto_fs_policy *policy);
void atto_fs_broker_close(atto_fs_broker *broker);

/* Operations read or write only regular files below the pinned root. Caller
 * must pass trusted scheduler identity; never expose this C ABI to guests.
 * No file descriptors, pointers, or privileged capabilities cross to guests. */
int atto_fs_broker_read(atto_fs_broker *broker, uint64_t tenant_id,
                        uint64_t generation, const char *relative_path,
                        unsigned char *output, size_t capacity, size_t *nread);
int atto_fs_broker_write(atto_fs_broker *broker, uint64_t tenant_id,
                         uint64_t generation, const char *relative_path,
                         const unsigned char *input, size_t length);
#ifdef __cplusplus
}
#endif
#endif
