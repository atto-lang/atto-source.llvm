#define _GNU_SOURCE
#include "atto/host_fs_async.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* HOST-ONLY live Linux broker + bounded worker test.
 * This is NOT compiled Atto source or proof of native TenantVM isolation. */
static void path_join(char *out, size_t size, const char *a, const char *b) {
    const int n = snprintf(out, size, "%s/%s", a, b);
    assert(n > 0 && (size_t)n < size);
}
static void write_file(const char *path, const char *s) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const size_t len = strlen(s);
    assert(write(fd, s, len) == (ssize_t)len);
    assert(close(fd) == 0);
}
static void expect_file(const char *path, const char *expected) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    char output[80] = {0};
    const ssize_t got = read(fd, output, sizeof(output));
    const size_t expected_len = strlen(expected);
    if (got != (ssize_t)expected_len) {
        fprintf(stderr, "ATTO_FS_ASYNC_BROKER_SIZE_MISMATCH path=%s got=%zd expected=%zu\\n",
                path, got, expected_len);
    }
    assert(got == (ssize_t)expected_len);
    assert(memcmp(output, expected, (size_t)got) == 0);
    assert(close(fd) == 0);
}
static int wait_ticket(atto_fs_async_service *service, atto_fs_host_actor *actor,
                       uint64_t ticket, uint8_t *out, size_t capacity, size_t *got) {
    for (unsigned i=0; i<1000000u; ++i) {
        const int rc = atto_fs_async_poll(service, actor, ticket, out, capacity, got);
        if (rc != -EAGAIN) return rc;
        if ((i & 255u) == 0u) sched_yield();
    }
    return -ETIMEDOUT;
}
int main(void) {
    char base[] = "/tmp/atto-fs-async-real-XXXXXX";
    assert(mkdtemp(base));
    char root[512], data[512], out[512], source[512], dest[512], outside[512];
    path_join(root, sizeof(root), base, "root"); assert(mkdir(root, 0700) == 0);
    path_join(data, sizeof(data), root, "data"); assert(mkdir(data, 0700) == 0);
    path_join(out, sizeof(out), root, "out"); assert(mkdir(out, 0700) == 0);
    path_join(source, sizeof(source), data, "input.txt");
    path_join(dest, sizeof(dest), out, "copy.txt");
    path_join(outside, sizeof(outside), base, "outside.txt");
    write_file(source, "host-async");
    write_file(outside, "outside");

    const atto_fs_layered_policy policy = {
        .tenant_id=77u, .generation=5u,
        .host={"*", "*", NULL, NULL},
        .artifact={"data", "out", NULL, NULL},
        .runtime={"data", "out", NULL, NULL},
        .tenant={"*", "*", NULL, NULL},
        .actor={"data", "out", NULL, NULL},
        .delegation={"*", "*", "data/private", "out/private"}
    };
    atto_fs_broker broker;
    assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
    atto_fs_host_actor actor = {77u, 5u, 123u, 6u};
    atto_fs_host_actor foreign = {77u, 5u, 999u, 6u};
    atto_fs_async_service *service = NULL;
    assert(atto_fs_async_create(&service, &broker, &actor, 4u) == 0);
    assert(atto_fs_async_forget_actor(service, &foreign) == -EACCES);
    uint64_t denied_ticket = 999u;
    assert(atto_fs_async_submit_read(service, &foreign, "data/input.txt",
                                      16u, &denied_ticket) == -EACCES);
    assert(denied_ticket == 0u);
    /* The broker's host worker can hold gate across Linux I/O. Poll-side
     * read publication must fail promptly rather than park a carrier:
     * holding the mutex on THIS test thread catches a blocking lock call. */
    unsigned char publication_probe[8];
    memset(publication_probe, 0xAC, sizeof(publication_probe));
    assert(pthread_mutex_lock(&broker.gate) == 0);
    assert(atto_fs_broker_publish_read(&broker, 77u, 5u,
                                       "data/input.txt", (const unsigned char *)"ok",
                                       2u, publication_probe, sizeof(publication_probe))
           == -EAGAIN);
    assert(publication_probe[0] == 0xAC);
    assert(pthread_mutex_unlock(&broker.gate) == 0);
    uint64_t ticket = 0;
    uint8_t bytes[64];
    memset(bytes, 0xAB, sizeof(bytes));
    size_t used = 123u;
    assert(atto_fs_async_submit_read(service, &actor, "data/input.txt", sizeof(bytes), &ticket)==0);
    assert(atto_fs_async_poll(service, &foreign, ticket, bytes, sizeof(bytes), &used)==-EACCES);
    assert(used==0u && bytes[0]==0xAB);
    assert(wait_ticket(service, &actor, ticket, bytes, sizeof(bytes), &used)==0);
    assert(used==10u && memcmp(bytes,"host-async",10)==0);
    assert(atto_fs_async_poll(service, &actor, ticket, bytes, sizeof(bytes), &used)==-ESTALE);
    assert(atto_fs_async_submit_write(service, &actor, "out/copy.txt", bytes, used, &ticket)==0);
    assert(wait_ticket(service, &actor, ticket, NULL, 0, &used)==0);
    expect_file(dest, "host-async");
    assert(atto_fs_async_submit_write(service, &actor, "data/input.txt",
                                      (const uint8_t*)"BAD",3,&ticket)==0);
    assert(wait_ticket(service, &actor, ticket, NULL, 0, &used)==-EACCES);
    expect_file(source, "host-async");
    assert(atto_fs_async_submit_read(service, &actor, "../outside.txt", sizeof(bytes), &ticket)==0);
    assert(wait_ticket(service, &actor, ticket, bytes, sizeof(bytes), &used)!=0);
    expect_file(outside, "outside");

    /* Whether the worker is queued or completed, final guest publication
     * after revocation is denied and never changes output bytes. */
    assert(atto_fs_async_submit_read(service, &actor, "data/input.txt", sizeof(bytes), &ticket)==0);
    assert(atto_fs_broker_revoke(&broker,77u,5u)==0);
    memset(bytes, 0xCD, sizeof(bytes));
    assert(wait_ticket(service, &actor, ticket, bytes, sizeof(bytes), &used)==-EACCES);
    assert(used==0u && bytes[0]==0xCD);
    assert(atto_fs_async_submit_write(service, &actor, "out/copy.txt",
                                      (const uint8_t*)"BAD",3,&ticket)==0);
    assert(wait_ticket(service, &actor, ticket, NULL, 0, &used)==-EACCES);
    expect_file(dest, "host-async");
    assert(atto_fs_async_forget_actor(service,&actor)==0);
    atto_fs_async_destroy(service);
    atto_fs_broker_close(&broker);
    puts("ATTO_FS_ASYNC_REAL_BROKER_HOST_ONLY_TEST_PASS");
    return 0;
}
