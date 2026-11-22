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
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct race_case {
    const atto_fs_trusted_guest_frame *frame;
    pthread_barrier_t *barrier;
    int saw_revoke;
} race_case;

static void *read_until_revoke(void *opaque) {
    race_case *r = (race_case *)opaque;
    int start = pthread_barrier_wait(r->barrier);
    assert(start == 0 || start == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i = 0; i < 10000u; ++i) {
        uint32_t got = 777u;
        const int rc = atto_fs_guest_read(r->frame, 16, 14, 768, 64, &got);
        if (rc == -EACCES) {
            r->saw_revoke = (got == 0u);
            return NULL;
        }
        assert(rc == 0 && got == 5u);
    }
    /* The revoker is guaranteed to run after barrier release; regardless of
     * scheduling, the late read must detect the revoked generation. */
    uint32_t got = 999u;
    assert(atto_fs_guest_read(r->frame, 16, 14, 768, 64, &got) == -EACCES);
    r->saw_revoke = (got == 0u);
    return NULL;
}

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

    atto_fs_layered_policy policy = {
        .tenant_id = 77u, .generation = 5u,
        .host = {"*", "*", NULL, NULL},
        .artifact = {"data", "out", NULL, NULL},
        .runtime = {"data", "out", NULL, NULL},
        .tenant = {"*", "*", NULL, NULL},
        .actor = {"data", "out", NULL, NULL},
        .delegation = {"*", "*", "data/secret.txt", "out/denied.txt"}
    };
    atto_fs_broker broker;
    assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
    uint8_t cage[4096]; memset(cage, 0xAB, sizeof(cage));
    memcpy(cage + 16, "data/input.txt", 14);
    memcpy(cage + 64, "out/copy.txt", 12);
    memcpy(cage + 96, "data/secret.txt", 15);
    memcpy(cage + 128, "out/denied.txt", 14);
    atto_fs_trusted_guest_frame frame = {&broker, 77u, 5u, cage, sizeof(cage), 16u, 2032u};
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
    /* The guest can address its own region, not another actor's region in
     * the SAME cage. Region start/end are trusted host-owned boundaries. */
    memcpy(cage + 3000, "data/input.txt", 14);
    assert(atto_fs_guest_read(&frame, 3000, 14, 256, 64, &count) == -EFAULT);
    assert(atto_fs_guest_read(&frame, 16, 14, 3000, 64, &count) == -EFAULT);
    assert(atto_fs_guest_write(&frame, 64, 12, 3000, 5) == -EFAULT);
    assert(atto_fs_guest_read(&frame, 0, 14, 256, 64, &count) == -EFAULT);
    /* A malformed host frame is rejected independently of caller's offsets. */
    frame.actor_region_bytes = UINT32_MAX;
    assert(atto_fs_guest_read(&frame, 16, 14, 256, 64, &count) == -EACCES);
    frame.actor_region_bytes = 2032u;
    memcpy(cage + 160, "data/\0secret", 12);
    assert(atto_fs_guest_read(&frame, 160, 12, 256, 64, &count) == -EINVAL);
    memcpy(cage + 176, "data/../x", 9);
    assert(atto_fs_guest_read(&frame, 176, 9, 256, 64, &count) == -EINVAL);
    file_expect(dst, "hello");
    file_expect(other, "outside");
    /* Revocation has the final authority even if a stale trusted frame remains. */
    /* Race host-side read completion with revocation. After revoke returns,
     * an outstanding worker must never copy an earlier read into the cage. */
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
    race_case racer = {&frame, &barrier, 0};
    pthread_t reader;
    assert(pthread_create(&reader, NULL, read_until_revoke, &racer) == 0);
    const int start = pthread_barrier_wait(&barrier);
    assert(start == 0 || start == PTHREAD_BARRIER_SERIAL_THREAD);
    assert(atto_fs_broker_revoke(&broker, 77u, 5u) == 0);
    memset(cage + 768, 0xC3, 64);
    assert(pthread_join(reader, NULL) == 0);
    assert(racer.saw_revoke == 1);
    for (size_t i = 0; i < 64; ++i) assert(cage[768 + i] == 0xC3);
    assert(pthread_barrier_destroy(&barrier) == 0);
    count = 999u;
    assert(atto_fs_guest_read(&frame, 16, 14, 512, 32, &count) == -EACCES);
    assert(count == 0u && cage[512] == 0xAB);
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
    file_expect(dst, "hello");
    atto_fs_broker_close(&broker);

    /* Six independent ceilings: a missing grant is a denial even when the
     * other five layers are broad and the file is writable at the OS level. */
    policy.runtime.allow_write = NULL;
    assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
    file_expect(dst, "hello");
    atto_fs_broker_close(&broker);
    policy.runtime.allow_write = "out";

    /* Every layer can deny regardless of more expansive allow grants in
     * the others, including a host-wide deny and child-delegated deny. */
    policy.host.deny_write = "out/copy.txt";
    assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
    file_expect(dst, "hello");
    atto_fs_broker_close(&broker);
    policy.host.deny_write = NULL;

    /* No policy-array alias remains: mutating a caller-owned scope after
     * admission cannot broaden or narrow an existing broker snapshot. */
    policy.runtime.allow_write = NULL;
    assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
    policy.runtime.allow_write = "out";
    assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
    atto_fs_broker_close(&broker);

    /* The one-layer legacy constructor cannot be smuggled through the
     * guest offset adapter as an apparently verified multi-layer policy. */
    const atto_fs_policy legacy = {"data", "out", NULL, NULL, 77u, 5u};
    assert(atto_fs_broker_init(&broker, root, &legacy) == 0);
    count = 999u;
    assert(atto_fs_guest_read(&frame, 16, 14, 512, 32, &count) == -EACCES);
    assert(count == 0u && cage[512] == 0xAB);
    atto_fs_broker_close(&broker);

    /* Prove each of the six independent ceilings is actually read by the
     * broker. Every missing allow and every matching explicit deny rejects,
     * rather than letting a different layer's broad allow override it. */
    atto_fs_policy_layer *ceilings[] = {
        &policy.host, &policy.artifact, &policy.runtime,
        &policy.tenant, &policy.actor, &policy.delegation
    };
    for (size_t i = 0; i < sizeof(ceilings) / sizeof(ceilings[0]); ++i) {
        atto_fs_policy_layer *layer = ceilings[i];
        const char *saved_allow_read = layer->allow_read;
        const char *saved_allow_write = layer->allow_write;
        const char *saved_deny_read = layer->deny_read;
        const char *saved_deny_write = layer->deny_write;
        layer->allow_read = NULL;
        assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
        count = 999u;
        assert(atto_fs_guest_read(&frame, 16, 14, 512, 32, &count) == -EACCES);
        assert(count == 0u && cage[512] == 0xAB);
        atto_fs_broker_close(&broker);
        layer->allow_read = saved_allow_read;

        layer->allow_write = NULL;
        assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
        assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
        file_expect(dst, "hello");
        atto_fs_broker_close(&broker);
        layer->allow_write = saved_allow_write;

        layer->deny_read = "data/input.txt";
        assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
        count = 999u;
        assert(atto_fs_guest_read(&frame, 16, 14, 512, 32, &count) == -EACCES);
        assert(count == 0u && cage[512] == 0xAB);
        atto_fs_broker_close(&broker);
        layer->deny_read = saved_deny_read;

        layer->deny_write = "out/copy.txt";
        assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
        assert(atto_fs_guest_write(&frame, 64, 12, 256, 5) == -EACCES);
        file_expect(dst, "hello");
        atto_fs_broker_close(&broker);
        layer->deny_write = saved_deny_write;
    }
    puts("ATTO_HOST_FS_GUEST_OFFSET_BRIDGE_HOST_ONLY_TEST_PASS");
    return 0;
}
