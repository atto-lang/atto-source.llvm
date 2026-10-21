#define _POSIX_C_SOURCE 200809L
#include "oreslang/runtime/NativeBlockingFileIo.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int checked_fd(int64_t fd) {
    return fd >= 0 && fd <= INT_MAX;
}

int64_t oreslang_file_write_i64_blocking(int64_t fd, int64_t offset, int64_t value) {
    if (!checked_fd(fd) || offset < 0) return 0;
    uint64_t bits = (uint64_t)value;
    unsigned char bytes[8];
    for (size_t i = 0; i < 8; ++i)
        bytes[i] = (unsigned char)((bits >> (i * 8u)) & 0xffu);

    size_t written = 0;
    while (written < sizeof(bytes)) {
        if (offset > INT64_MAX - (int64_t)written) return 0;
        const int64_t target = offset + (int64_t)written;
        const off_t at = (off_t)target;
        if (at < 0 || (int64_t)at != target) return 0;
        const ssize_t n = pwrite((int)fd, bytes + written,
                                 sizeof(bytes) - written, at);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0;
        written += (size_t)n;
    }
    return 1;
}

int64_t oreslang_file_size_blocking(int64_t fd) {
    if (!checked_fd(fd)) return -1;
    struct stat st;
    if (fstat((int)fd, &st) != 0 || st.st_size < 0) return -1;
    if ((uintmax_t)st.st_size > (uintmax_t)INT64_MAX) return -1;
    return (int64_t)st.st_size;
}

int64_t oreslang_file_truncate_blocking(int64_t fd, int64_t size) {
    if (!checked_fd(fd) || size < 0) return 0;
    const off_t target = (off_t)size;
    if (target < 0 || (int64_t)target != size) return 0;
    return ftruncate((int)fd, target) == 0 ? 1 : 0;
}

int64_t oreslang_file_sync_blocking(int64_t fd) {
    if (!checked_fd(fd)) return 0;
    for (;;) {
        if (fsync((int)fd) == 0) return 1;
        if (errno != EINTR) return 0;
    }
}

int64_t oreslang_dir_sync_blocking(int64_t fd) {
    // Directory descriptors are deliberately not opened by guest source. The
    // launcher grants this capability only for directories the process owns.
    return oreslang_file_sync_blocking(fd);
}
