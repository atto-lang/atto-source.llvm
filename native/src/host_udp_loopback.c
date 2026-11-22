#define _GNU_SOURCE
#include "atto/host_udp_loopback.h"
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_UDP4_PAYLOAD 65507u
struct atto_udp_loopback {
    pthread_mutex_t lock;
    int fd;
    unsigned requested_port;
    unsigned selected_port;
    atto_udp_authorize authorize;
    void *trusted_context;
};
static int admission(const atto_udp_loopback *s, unsigned port, int action) {
    /* Negative errno only; a positive return is never authorization. */
    return s->authorize(s->trusted_context, port, action) == 0 ? 0 : -EACCES;
}
static int gate(atto_udp_loopback *s) {
    if (!s) return -EINVAL;
    const int e = pthread_mutex_trylock(&s->lock);
    return e == 0 ? 0 : (e == EBUSY ? -EAGAIN : -e);
}
int atto_udp_loopback_open(atto_udp_loopback **out, unsigned port,
        atto_udp_authorize authorize, void *trusted_context) {
    if (out) *out = NULL;
    if (!out || !authorize || port > 65535u) return -EINVAL;
    if (authorize(trusted_context, port, ATTO_UDP_BIND) != 0) return -EACCES;
    atto_udp_loopback *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    int e = pthread_mutex_init(&s->lock, NULL);
    if (e) { free(s); return -e; }
    s->fd = -1;
    s->authorize = authorize;
    s->trusted_context = trusted_context;
    s->requested_port = port;
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { e = errno; goto error; }
    s->fd = fd;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { e = errno; goto error; }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) { e = errno; goto error; }
    if (len != sizeof(addr) || ntohl(addr.sin_addr.s_addr) != INADDR_LOOPBACK) { e=EACCES; goto error; }
    s->selected_port = ntohs(addr.sin_port);
    *out = s;
    return 0;
error:
    if (s->fd >= 0) close(s->fd);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return -e;
}
int atto_udp_loopback_port(atto_udp_loopback *s) {
    int rc = gate(s);
    if (rc) return rc;
    rc = s->fd < 0 ? -EBADF :
        admission(s, s->requested_port, ATTO_UDP_LOCAL);
    if (!rc) rc = (int)s->selected_port;
    pthread_mutex_unlock(&s->lock);
    return rc;
}
int atto_udp_loopback_send(atto_udp_loopback *s, unsigned port,
        const uint8_t *bytes, size_t size, size_t *sent) {
    if (sent) *sent = 0;
    if (!sent || !port || port > 65535u) return -EINVAL;
    int rc = gate(s);
    if (rc) return rc;
    if (s->fd < 0) rc = -EBADF;
    else if ((rc = admission(s, s->requested_port, ATTO_UDP_LOCAL)) == 0 &&
            (rc = admission(s, port, ATTO_UDP_SEND)) == 0) {
        /* A denied destination wins before inspecting caller payload size or
         * copying bytes. The host still validates both after authorization. */
        if (size > MAX_UDP4_PAYLOAD || (!bytes && size)) rc = -EINVAL;
        else {
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((uint16_t)port);
        const uint8_t empty = 0;
        const ssize_t n = sendto(s->fd, bytes ? bytes : &empty, size, MSG_DONTWAIT,
                                  (struct sockaddr *)&addr, sizeof(addr));
        if (n < 0) rc = -errno;
        else if ((size_t)n != size) rc = -EIO;
        else *sent = (size_t)n;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
int atto_udp_loopback_receive(atto_udp_loopback *s, uint8_t *out,
        size_t capacity, size_t *received, unsigned *source_port) {
    if (received) *received = 0;
    if (source_port) *source_port = 0;
    if (!received || !source_port || !out || !capacity || capacity > MAX_UDP4_PAYLOAD) return -EINVAL;
    int rc = gate(s);
    if (rc) return rc;
    if (s->fd < 0) rc = -EBADF;
    else if ((rc = admission(s, s->requested_port, ATTO_UDP_LOCAL)) == 0) {
        uint8_t *scratch = malloc(capacity + 1u);
        if (!scratch) rc = -ENOMEM;
        else {
            struct sockaddr_in peer = {0};
            struct iovec iov = { .iov_base = scratch, .iov_len = capacity + 1u };
            struct msghdr msg = {0};
            msg.msg_name = &peer;
            msg.msg_namelen = sizeof(peer);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            const ssize_t n = recvmsg(s->fd, &msg, MSG_DONTWAIT | MSG_TRUNC);
            if (n < 0) rc = -errno;
            else if (msg.msg_namelen != sizeof(peer) || peer.sin_family != AF_INET ||
                    ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK || !ntohs(peer.sin_port)) rc = -EACCES;
            else if ((rc = admission(s, ntohs(peer.sin_port), ATTO_UDP_PEER)) == 0) {
                if (n > (ssize_t)capacity || (msg.msg_flags & MSG_TRUNC)) rc = -EMSGSIZE;
                else { memcpy(out, scratch, (size_t)n); *received = (size_t)n; *source_port = ntohs(peer.sin_port); }
            }
            free(scratch);
        }
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
int atto_udp_loopback_close(atto_udp_loopback *s) {
    int rc = gate(s);
    if (rc) return rc;
    if (s->fd >= 0) {
        const int fd = s->fd;
        s->fd = -1;
        if (close(fd) != 0) rc = -errno;
    }
    pthread_mutex_unlock(&s->lock);
    return rc;
}
int atto_udp_loopback_destroy(atto_udp_loopback *s) {
    if (!s) return 0;
    /* A concurrent operation may be holding the trusted permission callback.
     * Never free the mutex or storage if it is still inside that operation.
     * The host must have fenced new users before attempting final destruction.
     */
    int rc = gate(s);
    if (rc) return rc;
    int fd = s->fd;
    s->fd = -1;
    pthread_mutex_unlock(&s->lock);
    if (fd >= 0) (void)close(fd);
    rc = pthread_mutex_destroy(&s->lock);
    if (rc) return -rc; /* fail closed: quarantine/retry, not use-after-free */
    free(s);
    return 0;
}
