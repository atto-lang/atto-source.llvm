#ifndef ATTO_HOST_FS_ASYNC_H
#define ATTO_HOST_FS_ASYNC_H
#include "atto/host_fs_broker.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* HOST-ONLY bounded single-worker I/O prototype. This is not a guest ABI.
 * Every actor context MUST be produced by an authenticated trusted scheduler;
 * never pass any of these integers or host pointers from a guest directly. */
#define ATTO_FS_ASYNC_MAX_JOBS 16u
typedef struct atto_fs_async_service atto_fs_async_service;
typedef struct atto_fs_host_actor {
    uint64_t tenant_id;
    uint64_t tenant_generation;
    uint64_t actor_ref;
    uint64_t actor_generation;
} atto_fs_host_actor;
/* A broker with ALL SIX layers is required. Service owns no root authority;
 * broker lifetime must outlive the service. Limit 1..16 pending/completed jobs.
 * Destroy only from host lifecycle management, NEVER on an actor carrier. */
int atto_fs_async_create(atto_fs_async_service **out, atto_fs_broker *broker, unsigned max_jobs);
void atto_fs_async_destroy(atto_fs_async_service *service);
/* Submit snapshots HOST-owned path and data before returning. The worker
 * never sees a guest cage pointer and never writes to guest memory. */
int atto_fs_async_submit_read(atto_fs_async_service *service, const atto_fs_host_actor *actor,
                              const char *host_path, size_t maximum_bytes, uint64_t *ticket);
int atto_fs_async_submit_write(atto_fs_async_service *service, const atto_fs_host_actor *actor,
                               const char *host_path, const uint8_t *host_bytes, size_t length,
                               uint64_t *ticket);
/* Carrier-safe polling: -EAGAIN while I/O is pending OR the service or
 * broker revocation mutex is busy. An -EAGAIN never consumes a ticket.
 * A read publishes its bytes ONLY under the broker's revoke lock after a
 * renewed authorization check. output is HOST-VALIDATED actor memory;
 * the caller must check actor-private offsets and live incarnation first. */
int atto_fs_async_poll(atto_fs_async_service *service, const atto_fs_host_actor *actor,
                       uint64_t ticket, uint8_t *host_validated_output, size_t capacity,
                       size_t *bytes_read);
/* Cancellation of a queued job prevents its side effect. Cancellation of an
 * already executing write CANNOT undo its side effect; poll returns CANCELLED
 * only after the underlying operation finishes. */
int atto_fs_async_cancel(atto_fs_async_service *service, const atto_fs_host_actor *actor,
                         uint64_t ticket);
/* Abandon all tickets of a dead actor incarnation. In-flight jobs finish in
 * host memory and are discarded, never committed to reused actor memory. */
int atto_fs_async_forget_actor(atto_fs_async_service *service,
                               const atto_fs_host_actor *actor);
#ifdef __cplusplus
}
#endif
#endif
