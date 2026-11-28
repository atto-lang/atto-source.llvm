#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeBlockingFile.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

static int valid_basename(const char* path) {
    if (!path || !path[0]) return 0;
    const size_t n = strlen(path);
    if (n > 255 || (n == 1 && path[0] == '.') ||
            (n == 2 && path[0] == '.' && path[1] == '.')) return 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)path[i];
        const int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const int digit = c >= '0' && c <= '9';
        if (!(alpha || digit || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int64_t checked_regular_fd(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0) return ORES_NATIVE_FILE_IO_ERROR;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) return ORES_NATIVE_FILE_SECURITY;
    return 1;
}

static int64_t open_checked(const char* basename, int flags, int replace) {
    if (!valid_basename(basename)) return ORES_NATIVE_FILE_INVALID;
    int fd;
    do {
        fd = openat(AT_FDCWD, basename, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        if (errno == ELOOP) return ORES_NATIVE_FILE_SECURITY;
        return ORES_NATIVE_FILE_IO_ERROR;
    }
    const int64_t checked = checked_regular_fd(fd);
    if (checked != 1) {
        (void)close(fd);
        return checked;
    }
    // Never open with O_TRUNC: a preexisting hardlink must be rejected before
    // any destructive mutation. Truncate only after regular/single-link proof.
    if (replace && ftruncate(fd, 0) != 0) {
        (void)close(fd);
        return ORES_NATIVE_FILE_IO_ERROR;
    }
    return (int64_t)fd;
}

int64_t oreslang_file_open_read_blocking(const char* basename) {
    return open_checked(basename, O_RDONLY, 0);
}

int64_t oreslang_file_open_append_blocking(const char* basename) {
    return open_checked(basename, O_WRONLY | O_APPEND | O_CREAT, 0);
}

int64_t oreslang_file_open_replace_blocking(const char* basename) {
    return open_checked(basename, O_WRONLY | O_CREAT, 1);
}

static int valid_fd(int64_t fd) {
    return fd >= 0 && fd <= 0x7fffffff;
}

int64_t oreslang_file_write_u63_blocking(int64_t fd_value, int64_t value) {
    if (!valid_fd(fd_value) || value < 0) return ORES_NATIVE_FILE_INVALID;
    const int fd = (int)fd_value;
    if (checked_regular_fd(fd) != 1) return ORES_NATIVE_FILE_SECURITY;
    unsigned char bytes[8];
    const uint64_t v = (uint64_t)value;
    for (unsigned i = 0; i < 8; ++i)
        bytes[7u - i] = (unsigned char)((v >> (i * 8u)) & 0xffu);
    size_t at = 0;
    while (at < sizeof(bytes)) {
        const ssize_t n = write(fd, bytes + at, sizeof(bytes) - at);
        if (n > 0) {
            at += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return ORES_NATIVE_FILE_IO_ERROR;
    }
    return 1;
}

int64_t oreslang_file_read_u63_blocking(int64_t fd_value) {
    if (!valid_fd(fd_value)) return ORES_NATIVE_FILE_INVALID;
    const int fd = (int)fd_value;
    if (checked_regular_fd(fd) != 1) return ORES_NATIVE_FILE_SECURITY;
    unsigned char bytes[8];
    size_t at = 0;
    while (at < sizeof(bytes)) {
        const ssize_t n = read(fd, bytes + at, sizeof(bytes) - at);
        if (n > 0) {
            at += (size_t)n;
            continue;
        }
        if (n == 0) return at == 0 ? ORES_NATIVE_FILE_EOF : ORES_NATIVE_FILE_CORRUPT;
        if (errno == EINTR) continue;
        return ORES_NATIVE_FILE_IO_ERROR;
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8u) | bytes[i];
    if (value > INT64_MAX) return ORES_NATIVE_FILE_CORRUPT;
    return (int64_t)value;
}

int64_t oreslang_file_sync_blocking(int64_t fd_value) {
    if (!valid_fd(fd_value)) return ORES_NATIVE_FILE_INVALID;
    const int fd = (int)fd_value;
    if (checked_regular_fd(fd) != 1) return ORES_NATIVE_FILE_SECURITY;
    int rc;
    do { rc = fsync(fd); } while (rc != 0 && errno == EINTR);
    return rc == 0 ? 1 : ORES_NATIVE_FILE_IO_ERROR;
}

int64_t oreslang_file_close_blocking(int64_t fd_value) {
    if (!valid_fd(fd_value)) return ORES_NATIVE_FILE_INVALID;
    int rc;
    do { rc = close((int)fd_value); } while (rc != 0 && errno == EINTR);
    return rc == 0 ? 1 : ORES_NATIVE_FILE_IO_ERROR;
}

static int64_t checked_path_state(const char* basename, int may_be_absent) {
    if (!valid_basename(basename)) return ORES_NATIVE_FILE_INVALID;
    struct stat st;
    if (fstatat(AT_FDCWD, basename, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (may_be_absent && errno == ENOENT) return 0;
        return ORES_NATIVE_FILE_IO_ERROR;
    }
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1) return ORES_NATIVE_FILE_SECURITY;
    return 1;
}

int64_t oreslang_file_rename_blocking(const char* from_basename, const char* to_basename) {
    const int64_t from = checked_path_state(from_basename, 0);
    if (from != 1) return from;
    const int64_t to = checked_path_state(to_basename, 1);
    if (to < 0) return to;
    int rc;
    do {
        rc = renameat(AT_FDCWD, from_basename, AT_FDCWD, to_basename);
    } while (rc != 0 && errno == EINTR);
    return rc == 0 ? 1 : ORES_NATIVE_FILE_IO_ERROR;
}

int64_t oreslang_dir_sync_cwd_blocking(void) {
    int fd;
    do {
        fd = openat(AT_FDCWD, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return ORES_NATIVE_FILE_IO_ERROR;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        (void)close(fd);
        return ORES_NATIVE_FILE_SECURITY;
    }
    int rc;
    do { rc = fsync(fd); } while (rc != 0 && errno == EINTR);
    const int saved = errno;
    int close_rc;
    do { close_rc = close(fd); } while (close_rc != 0 && errno == EINTR);
    if (rc != 0) {
        errno = saved;
        return ORES_NATIVE_FILE_IO_ERROR;
    }
    return close_rc == 0 ? 1 : ORES_NATIVE_FILE_IO_ERROR;
}
