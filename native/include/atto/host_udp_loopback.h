#ifndef ATTO_HOST_UDP_LOOPBACK_H
#define ATTO_HOST_UDP_LOOPBACK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Linux host-only prototype, not a guest ABI or permission grant.
 * The trusted host supplies an unforgeable callback/context and retains them
 * until destroy. Never accept a guest-provided callback or expose fd/pointers. */
typedef struct atto_udp_loopback atto_udp_loopback;
enum { ATTO_UDP_BIND=1, ATTO_UDP_LOCAL=2, ATTO_UDP_SEND=3, ATTO_UDP_PEER=4 };
typedef int (*atto_udp_authorize)(void *trusted_context, unsigned port, int operation);
/* Bind IPv4 127.0.0.1 only. port=0 is an ephemeral bind, not an egress grant.
 * All success is 0; errors are negative errno. No internal I/O worker. */
int atto_udp_loopback_open(atto_udp_loopback **out, unsigned port,
        atto_udp_authorize authorize, void *trusted_context);
int atto_udp_loopback_port(atto_udp_loopback *socket);
/* Complete packet or error; zero-byte datagrams are valid.
 * sent and received sizes are always set to zero on error. */
int atto_udp_loopback_send(atto_udp_loopback *socket, unsigned peer_port,
        const uint8_t *bytes, size_t size, size_t *sent);
int atto_udp_loopback_receive(atto_udp_loopback *socket, uint8_t *owned_output,
        size_t capacity, size_t *received, unsigned *source_port);
/* Close is idempotent. If another syscall holds the gate returns -EAGAIN.
 * Destroy ONLY after the host fences new entrants and callers have stopped.
 * Return -EAGAIN without freeing if the lock is still held, allowing a safe
 * retry instead of freeing a live mutex/descriptor. */
int atto_udp_loopback_close(atto_udp_loopback *socket);
int atto_udp_loopback_destroy(atto_udp_loopback *socket);
#ifdef __cplusplus
}
#endif
#endif
