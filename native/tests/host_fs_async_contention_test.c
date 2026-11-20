#define _GNU_SOURCE
/* Intentional single-TU internal-state test: verifies that actor-facing
 * submit/poll/cancel use trylock even if a trusted host thread holds the
 * queue mutex. Do not compile the real broker into this mock-only test. */
#include "../src/host_fs_async.c"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t worker_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_ready = PTHREAD_COND_INITIALIZER;
static int hold_worker = 1;
static int worker_started = 0;
static int broker_read_count = 0;
static int broker_write_count = 0;

int atto_fs_broker_read(atto_fs_broker *broker, uint64_t tenant, uint64_t generation,
                        const char *path, unsigned char *output, size_t cap, size_t *got) {
    assert(broker && tenant == 7u && generation == 3u);
    assert(strcmp(path, "data/input") == 0);
    pthread_mutex_lock(&worker_gate);
    worker_started = 1;
    pthread_cond_broadcast(&worker_ready);
    while (hold_worker) pthread_cond_wait(&worker_ready, &worker_gate);
    ++broker_read_count;
    pthread_mutex_unlock(&worker_gate);
    assert(cap >= 4u);
    memcpy(output, "data", 4u);
    *got = 4u;
    return 0;
}
int atto_fs_broker_write(atto_fs_broker *broker, uint64_t tenant, uint64_t generation,
                         const char *path, const unsigned char *bytes, size_t len) {
    (void)broker; (void)tenant; (void)generation;
    (void)path; (void)bytes; (void)len;
    ++broker_write_count;
    return 0;
}
int atto_fs_broker_publish_read(atto_fs_broker *broker, uint64_t tenant, uint64_t generation,
                                const char *path, const unsigned char *bytes, size_t len,
                                unsigned char *out, size_t capacity) {
    (void)broker; (void)tenant; (void)generation; (void)path;
    if (len > capacity) return -EMSGSIZE;
    memcpy(out, bytes, len);
    return 0;
}

static int poll_until_finished(atto_fs_async_service *service,
                               const atto_fs_host_actor *actor, uint64_t ticket,
                               unsigned char *out, size_t *len) {
    for (unsigned n = 0u; n < 2000000u; ++n) {
        const int rc = atto_fs_async_poll(service, actor, ticket, out, 16u, len);
        if (rc != -EAGAIN) return rc;
        if ((n & 511u) == 0u) sched_yield();
    }
    return -ETIMEDOUT;
}
int main(void) {
    atto_fs_broker broker = {0};
    broker.policy_layer_count = ATTO_FS_POLICY_LAYER_COUNT;
    broker.tenant_id = 7u;
    broker.generation = 3u;
    const atto_fs_host_actor actor = {7u,3u,55u,9u};
    const atto_fs_host_actor sibling = {7u,3u,56u,9u};
    const atto_fs_host_actor reincarnated = {7u,3u,55u,10u};
    atto_fs_async_service *service = NULL;
    assert(atto_fs_async_create(&service, &broker, &actor, 1u) == 0);
    /* A tenant match alone is never enough: the service is bound to this
     * exact trusted actor incarnation for its complete lifetime. */
    uint64_t denied_ticket = 991u;
    assert(atto_fs_async_submit_read(service, &sibling, "data/input", 16u, &denied_ticket)
           == -EACCES);
    assert(denied_ticket == 0u);
    assert(atto_fs_async_submit_read(service, &reincarnated, "data/input", 16u,
                                     &denied_ticket) == -EACCES);
    assert(denied_ticket == 0u);
    assert(atto_fs_async_cancel(service, &reincarnated, 1u) == -EACCES);
    assert(atto_fs_async_cancel(service, &sibling, 1u) == -EACCES);
    unsigned char foreign_output[8];
    memset(foreign_output, 0xA5, sizeof foreign_output);
    size_t foreign_used = 99u;
    assert(atto_fs_async_poll(service, &sibling, 1u, foreign_output,
                              sizeof foreign_output, &foreign_used) == -EACCES);
    assert(foreign_used == 0u && foreign_output[0] == 0xA5);
    assert(atto_fs_async_poll(service, &reincarnated, 1u, foreign_output,
                              sizeof foreign_output, &foreign_used) == -EACCES);
    assert(foreign_used == 0u && foreign_output[0] == 0xA5);
    assert(atto_fs_async_forget_actor(service, &sibling) == -EACCES);
    unsigned char output[16];
    memset(output, 0x9a, sizeof output);
    size_t got = 123u;
    uint64_t ticket = 456u;

    /* An actor-facing API MUST return promptly even when its own thread
     * holds the service mutex. A blocking lock would deadlock this test. */
    assert(pthread_mutex_lock(&service->lock) == 0);
    assert(atto_fs_async_submit_read(service,&actor,"data/input",16u,&ticket)==-EAGAIN);
    assert(ticket == 0u);
    assert(atto_fs_async_submit_write(service,&actor,"out/copy",
                                      (const uint8_t *)"x",1u,&ticket)==-EAGAIN);
    assert(ticket == 0u);
    assert(atto_fs_async_poll(service,&actor,1u,output,sizeof output,&got)==-EAGAIN);
    assert(got == 0u && output[0] == 0x9a);
    assert(atto_fs_async_cancel(service,&actor,1u)==-EAGAIN);
    assert(pthread_mutex_unlock(&service->lock) == 0);

    assert(atto_fs_async_submit_read(service,&actor,"data/input",16u,&ticket)==0);
    assert(ticket != 0u);
    pthread_mutex_lock(&worker_gate);
    while (!worker_started) pthread_cond_wait(&worker_ready,&worker_gate);
    pthread_mutex_unlock(&worker_gate);

    /* Fill the queue with a running ticket. Every additional submit MUST
     * fail without issuing any actual broker write or returning a ticket. */
    const uint8_t one[] = {1u};
    uint64_t rejected = 999u;
    assert(atto_fs_async_submit_write(service,&actor,"out/copy",one,sizeof one,&rejected)==-EAGAIN);
    assert(rejected==0u && broker_write_count==0);

    pthread_mutex_lock(&worker_gate);
    hold_worker = 0;
    pthread_cond_broadcast(&worker_ready);
    pthread_mutex_unlock(&worker_gate);
    assert(poll_until_finished(service,&actor,ticket,output,&got)==0);
    assert(got==4u && memcmp(output,"data",got)==0);
    assert(broker_read_count==1 && broker_write_count==0);
    assert(atto_fs_async_poll(service,&actor,ticket,output,sizeof output,&got)==-ESTALE);
    atto_fs_async_destroy(service);
    puts("ATTO_FS_ASYNC_SUBMIT_CONTENTION_HOST_ONLY_PASS");
    return 0;
}
