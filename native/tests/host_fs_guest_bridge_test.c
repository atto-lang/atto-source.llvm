#define _GNU_SOURCE
#include "atto/host_fs_guest_bridge.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void file_write(const char *path, const char *text) {
    int fd = open(path, O_CLOEXEC | O_CREAT | O_TRUNC | O_WRONLY, 0600);
    assert(fd >= 0);
    const size_t n = strlen(text);
    assert(write(fd, text, n) == (ssize_t)n);
    assert(close(fd) == 0);
}
static void file_expect(const char *path, const char *text) {
    char buffer[64] = {0};
    int fd = open(path, O_CLOEXEC | O_RDONLY); assert(fd >= 0);
    const ssize_t n = read(fd, buffer, sizeof(buffer));
    assert(n == (ssize_t)strlen(text));
    assert(memcmp(buffer, text, (size_t)n) == 0);
    assert(close(fd) == 0);
}
static void join(char *dst, size_t cap, const char *base, const char *part) {
    assert(snprintf(dst, cap, "%s/%s", base, part) < (int)cap);
}
int main(void) {
    char home[] = "/tmp/atto-fs-guest-bridge-XXXXXX";
    assert(mkdtemp(home));
    char root[400], data[400], out[400], src[400], dst[400], other[400];
    join(root, sizeof(root), home, "root"); assert(mkdir(root, 0700) == 0);
    join(data, sizeof(data), root, "data"); assert(mkdir(data, 0700) == 0);
    join(out, sizeof(out), root, "out"); assert(mkdir(out, 0700) == 0);
    join(src, sizeof(src), data, "input.txt"); file_write(src, "hello");
    join(dst, sizeof(dst), out, "copy.txt");
    join(other, sizeof(other), home, "outside.txt"); file_write(other, "outside");

    const atto_fs_policy policy = {"data", "out", "data/secret.txt", "out/denied.txt", 77u, 5u};
    atto_fs_broker broker;
    assert(atto_fs_broker_init(&broker, root, &policy) == 0);
    uint8_t cage[4096]; memset(cage, 0xAB, sizeof(cage));
    memcpy(cage + 16, "data/input.txt", 14);
    memcpy(cage + 64, "out/copy.txt", 12);
    memcpy(cage + 96, "data/secret.txt", 15);
    memcpy(cage + 128, "out/denied.txt", 14);
    atto_fs_trusted_guest_frame frame = {&broker, 77u, 5u, cage, sizeof(cage)};
    uint32_t count = 999u;
    assert(atto_fs_guest_read(&frame, 16, 14, 256, 64, &count) == 0);
    assert(count == 5u && memcmp(cage + 256, "hello", 5) == 0);
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == 0);
    file_expect(dst, "hello");
    /* Denies must not write into cage or create host files. */
    count = 999u;
    assert(atto_fs_guest_read(&frame, 96, 15, 512, 32, &count) == -EACCES);
    assert(count == 0u && cage[512] == 0xAB);
    assert(atto_fs_guest_write(&frame, 128, 14, 256, 5) == -EACCES);
    char denied[400]; join(denied, sizeof(denied), out, "denied.txt");
    assert(access(denied, F_OK) != 0);
    /* Guest data cannot manufacture another tenant or generation. */
    frame.tenant_id = 78u;
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
    frame.tenant_id = 77u;
    frame.generation = 6u;
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
    frame.generation = 5u;
    /* No offset wrap, raw host pointer, NUL injection, or oversized length. */
    assert(atto_fs_guest_read(&frame, UINT32_MAX, 14, 256, 64, &count) == -EFAULT);
    assert(atto_fs_guest_read(&frame, 16, 14, 4090, 32, &count) == -EFAULT);
    assert(atto_fs_guest_read(&frame, 16, 14, 256, ATTO_FS_MAX_BYTES + 1u, &count) == -EINVAL);
    assert(atto_fs_guest_write(&frame, 64, 12, UINT32_MAX, 9) == -EFAULT);
    memcpy(cage + 160, "data/\0secret", 12);
    assert(atto_fs_guest_read(&frame, 160, 12, 256, 64, &count) == -EINVAL);
    memcpy(cage + 176, "data/../x", 9);
    assert(atto_fs_guest_read(&frame, 176, 9, 256, 64, &count) == -EINVAL);
    file_expect(dst, "hello");
    file_expect(other, "outside");
    /* Revocation has the final authority even if a stale trusted frame remains. */
    assert(atto_fs_broker_revoke(&broker, 77u, 5u) == 0);
    count = 999u;
    assert(atto_fs_guest_read(&frame, 16, 14, 512, 32, &count) == -EACCES);
    assert(count == 0u && cage[512] == 0xAB);
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
    file_expect(dst, "hello");
    atto_fs_broker_close(&broker);
    puts("ATTO_HOST_FS_GUEST_OFFSET_BRIDGE_HOST_ONLY_TEST_PASS");
    return 0;
}
