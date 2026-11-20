#define _GNU_SOURCE
#include "atto/host_fs_broker.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* This implementation targets Linux openat2/renameat, NOT portable UNIX.
 * Resolve policy closes symlinks, magic links and bind/mount traversal.
 * It does not sandbox native code that can directly issue syscalls. */
static int valid_rel(const char *text) {
    if (!text || !*text || *text == '/' || *text == '-' ||
        strnlen(text, ATTO_FS_SCOPE_LIMIT) >= ATTO_FS_SCOPE_LIMIT) return 0;
    const char *start = text;
    for (const char *p = text;; ++p) {
        const unsigned char c = (unsigned char)*p;
        if ((c != 0 && c < 32) || c == 127 || c == '\\' || c == ':') return 0;
        if (c == '/' || c == '\0') {
            const size_t n = (size_t)(p - start);
            if (n == 0 || (n == 1 && start[0] == '.') ||
                (n == 2 && start[0] == '.' && start[1] == '.')) return 0;
            if (c == '\0') break;
            start = p + 1;
        }
    }
    return 1;
}
static int valid_scope(const char *text) {
    return text == NULL || (strcmp(text, "*") == 0) || valid_rel(text);
}
static void copy_scope(char dest[ATTO_FS_SCOPE_LIMIT], const char *source) {
    if (!source) { dest[0] = '\0'; return; }
    (void)snprintf(dest, ATTO_FS_SCOPE_LIMIT, "%s", source);
}
static int matches(const char *scope, const char *path) {
    if (scope[0] == '\0') return 0;
    if (strcmp(scope, "*") == 0) return 1;
    const size_t n = strlen(scope);
    return strlen(path) >= n && strncmp(scope, path, n) == 0 && (path[n] == '/' || path[n] == '\0');
}
static int authorized(atto_fs_broker *b, uint64_t tenant, uint64_t generation,
                      const char *path, int write) {
    if (!b || !b->initialized || b->revoked || b->root_fd < 0 || !tenant || !generation ||
        tenant != b->tenant_id || generation != b->generation) return -EACCES;
    if (!valid_rel(path)) return -EINVAL;
    if (b->policy_layer_count == ATTO_FS_POLICY_LAYER_COUNT) {
        for (uint32_t i = 0; i < ATTO_FS_POLICY_LAYER_COUNT; ++i) {
            const char *allow = write ? b->write_allow_layers[i] : b->read_allow_layers[i];
            const char *deny = write ? b->write_deny_layers[i] : b->read_deny_layers[i];
            if (!matches(allow, path) || matches(deny, path)) return -EACCES;
        }
        return 0;
    }
    /* Legacy one-layer constructor: host-only compatibility, NEVER a
     * sufficient guest permission manifest. The guest bridge rejects it. */
    const char *allow = write ? b->write_scope : b->read_scope;
    const char *deny = write ? b->deny_write_scope : b->deny_read_scope;
    if (!matches(allow, path) || matches(deny, path)) return -EACCES;
    return 0;
}
static int open_beneath(int root_fd, const char *path, int flags, mode_t mode) {
    struct open_how how = {0};
    how.flags = (__u64)(unsigned int)(flags | O_CLOEXEC | O_NOFOLLOW);
    how.mode = (__u64)mode;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    const long fd = syscall(SYS_openat2, root_fd, path, &how, sizeof(how));
    if (fd < 0) return -errno;
    return (int)fd;
}
int atto_fs_broker_init(atto_fs_broker *out, const char *trusted_root,
                        const atto_fs_policy *policy) {
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    out->root_fd = -1;
    if (!trusted_root || trusted_root[0] != '/' || !policy ||
        !policy->tenant_id || !policy->generation ||
        !valid_scope(policy->allow_read) || !valid_scope(policy->allow_write) ||
        !valid_scope(policy->deny_read) || !valid_scope(policy->deny_write)) return -EINVAL;
    const int fd = open(trusted_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        const int err = errno ? errno : EINVAL;
        (void)close(fd);
        return -err;
    }
    /* No guest supplied root path; host has to validate its directory origin. */
    int gate_code = pthread_mutex_init(&out->gate, NULL);
    if (gate_code) { (void)close(fd); return -gate_code; }
    out->initialized = 1;
    out->revoked = 0;
    out->root_fd = fd;
    out->tenant_id = policy->tenant_id;
    out->generation = policy->generation;
    copy_scope(out->read_scope, policy->allow_read);
    copy_scope(out->write_scope, policy->allow_write);
    copy_scope(out->deny_read_scope, policy->deny_read);
    copy_scope(out->deny_write_scope, policy->deny_write);
    return 0;
}

int atto_fs_broker_init_layers(atto_fs_broker *out, const char *trusted_root,
                               const atto_fs_layered_policy *policy) {
    if (!out || !policy) return -EINVAL;
    const atto_fs_policy_layer *layers[ATTO_FS_POLICY_LAYER_COUNT] = {
        &policy->host, &policy->artifact, &policy->runtime,
        &policy->tenant, &policy->actor, &policy->delegation
    };
    for (uint32_t i = 0; i < ATTO_FS_POLICY_LAYER_COUNT; ++i) {
        const atto_fs_policy_layer *layer = layers[i];
        if (!valid_scope(layer->allow_read) || !valid_scope(layer->allow_write) ||
            !valid_scope(layer->deny_read) || !valid_scope(layer->deny_write)) return -EINVAL;
    }
    const atto_fs_policy first = {
        policy->host.allow_read, policy->host.allow_write,
        policy->host.deny_read, policy->host.deny_write,
        policy->tenant_id, policy->generation
    };
    const int rc = atto_fs_broker_init(out, trusted_root, &first);
    if (rc) return rc;
    for (uint32_t i = 0; i < ATTO_FS_POLICY_LAYER_COUNT; ++i) {
        const atto_fs_policy_layer *layer = layers[i];
        copy_scope(out->read_allow_layers[i], layer->allow_read);
        copy_scope(out->write_allow_layers[i], layer->allow_write);
        copy_scope(out->read_deny_layers[i], layer->deny_read);
        copy_scope(out->write_deny_layers[i], layer->deny_write);
    }
    out->policy_layer_count = ATTO_FS_POLICY_LAYER_COUNT;
    return 0;
}
int atto_fs_broker_revoke(atto_fs_broker *b, uint64_t tenant,
                          uint64_t generation) {
    if (!b || !b->initialized) return -EACCES;
    const int rc = pthread_mutex_lock(&b->gate);
    if (rc) return -rc;
    const int result = (!b->revoked && b->root_fd >= 0 && tenant && generation &&
                        tenant == b->tenant_id && generation == b->generation)
        ? 0 : -EACCES;
    if (!result) {
        b->revoked = 1;
        b->generation = 0;
    }
    (void)pthread_mutex_unlock(&b->gate);
    return result;
}
void atto_fs_broker_close(atto_fs_broker *b) {
    if (!b || !b->initialized) return;
    /* Host lifecycle guarantees no concurrently starting worker and no use
     * after close. Revoke first when running actors may still send requests. */
    if (pthread_mutex_lock(&b->gate) != 0) return;
    b->revoked = 1;
    if (b->root_fd >= 0) (void)close(b->root_fd);
    b->root_fd = -1;
    b->tenant_id = 0;
    b->generation = 0;
    memset(b->read_scope, 0, sizeof(b->read_scope));
    memset(b->write_scope, 0, sizeof(b->write_scope));
    memset(b->deny_read_scope, 0, sizeof(b->deny_read_scope));
    memset(b->deny_write_scope, 0, sizeof(b->deny_write_scope));
    memset(b->read_allow_layers, 0, sizeof(b->read_allow_layers));
    memset(b->write_allow_layers, 0, sizeof(b->write_allow_layers));
    memset(b->read_deny_layers, 0, sizeof(b->read_deny_layers));
    memset(b->write_deny_layers, 0, sizeof(b->write_deny_layers));
    b->policy_layer_count = 0;
    (void)pthread_mutex_unlock(&b->gate);
    (void)pthread_mutex_destroy(&b->gate);
    b->initialized = 0;
}
static int read_locked(atto_fs_broker *b, uint64_t tenant, uint64_t gen,
                        const char *path, unsigned char *output,
                        size_t capacity, size_t *nread) {
    if (nread) *nread = 0;
    if (!output || !nread || capacity > ATTO_FS_MAX_BYTES) return -EINVAL;
    const int auth = authorized(b, tenant, gen, path, 0);
    if (auth) return auth;
    const int fd = open_beneath(b->root_fd, path, O_RDONLY, 0);
    if (fd < 0) return fd;
    struct stat st;
    int code = 0;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)capacity) {
        code = -EACCES;
    } else {
        unsigned char *tmp = (unsigned char *)malloc(capacity + 1u);
        if (!tmp) { code = -ENOMEM; }
        else {
            size_t got = 0;
            while (got <= capacity) {
                const ssize_t n = read(fd, tmp + got, capacity + 1u - got);
                if (n < 0) { if (errno == EINTR) continue; code = -errno; break; }
                if (n == 0) break;
                got += (size_t)n;
                if (got > capacity) { code = -EFBIG; break; }
            }
            if (!code) { memcpy(output, tmp, got); *nread = got; }
            free(tmp);
        }
    }
    (void)close(fd);
    return code;
}

/* Obtain a trusted open parent FD so rename acts on directory inodes, not a
 * pathname re-resolved after validation. A hostile process able to move
 * whole directories concurrently is outside this cooperative host model. */
static int open_parent(atto_fs_broker *b, const char *path,
                       char basename[ATTO_FS_SCOPE_LIMIT]) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        (void)snprintf(basename, ATTO_FS_SCOPE_LIMIT, "%s", path);
        return fcntl(b->root_fd, F_DUPFD_CLOEXEC, 3);
    }
    const size_t n = (size_t)(slash - path);
    char parent[ATTO_FS_SCOPE_LIMIT];
    memcpy(parent, path, n);
    parent[n] = '\0';
    (void)snprintf(basename, ATTO_FS_SCOPE_LIMIT, "%s", slash + 1);
    return open_beneath(b->root_fd, parent, O_RDONLY | O_DIRECTORY, 0);
}
static int write_locked(atto_fs_broker *b, uint64_t tenant, uint64_t gen,
                         const char *path, const unsigned char *input,
                         size_t length) {
    if ((!input && length) || length > ATTO_FS_MAX_BYTES) return -EINVAL;
    const int auth = authorized(b, tenant, gen, path, 1);
    if (auth) return auth;
    char leaf[ATTO_FS_SCOPE_LIMIT];
    int dirfd = open_parent(b, path, leaf);
    if (dirfd < 0) return dirfd == -1 ? -errno : dirfd;
    /* Forbid pre-existing symbolic links / directories and multi-link files.
     * Temp+rename means existing hardlink targets are never truncated. */
    struct stat old;
    if (fstatat(dirfd, leaf, &old, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(old.st_mode) || old.st_nlink != 1) {
            (void)close(dirfd); return -EACCES;
        }
    } else if (errno != ENOENT) {
        const int err = errno; (void)close(dirfd); return -err;
    }
    int fd = -1;
    char temp[48] = {0};
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
        unsigned char random[16];
        if (getrandom(random, sizeof(random), 0) != (ssize_t)sizeof(random)) {
            (void)close(dirfd); return -EIO;
        }
        for (size_t j = 0; j < sizeof(random); ++j)
            (void)snprintf(temp + 8u + j * 2u, sizeof(temp) - (8u + j * 2u), "%02x", random[j]);
        memcpy(temp, ".atto-t-", 8);
        fd = openat(dirfd, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) break;
    }
    if (fd < 0) { const int err = errno; (void)close(dirfd); return -err; }
    int code = 0;
    size_t total = 0;
    while (total < length) {
        const ssize_t n = write(fd, input + total, length - total);
        if (n < 0) { if (errno == EINTR) continue; code = -errno; break; }
        if (n == 0) { code = -EIO; break; }
        total += (size_t)n;
    }
    if (!code && fsync(fd) != 0) code = -errno;
    if (close(fd) != 0 && !code) code = -errno;
    if (!code && renameat(dirfd, temp, dirfd, leaf) != 0) code = -errno;
    if (code) (void)unlinkat(dirfd, temp, 0);
    (void)close(dirfd);
    return code;
}

int atto_fs_broker_read(atto_fs_broker *b, uint64_t tenant, uint64_t gen,
                        const char *path, unsigned char *output,
                        size_t capacity, size_t *nread) {
    if (nread) *nread = 0;
    if (!b || !b->initialized) return -EACCES;
    const int rc = pthread_mutex_lock(&b->gate);
    if (rc) return -rc;
    const int result = read_locked(b, tenant, gen, path, output, capacity, nread);
    (void)pthread_mutex_unlock(&b->gate);
    return result;
}
int atto_fs_broker_write(atto_fs_broker *b, uint64_t tenant, uint64_t gen,
                         const char *path, const unsigned char *input, size_t length) {
    if (!b || !b->initialized) return -EACCES;
    const int rc = pthread_mutex_lock(&b->gate);
    if (rc) return -rc;
    const int result = write_locked(b, tenant, gen, path, input, length);
    (void)pthread_mutex_unlock(&b->gate);
    return result;
}
