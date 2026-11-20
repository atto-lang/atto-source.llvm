#define _GNU_SOURCE
#include "atto/host_fs_guest_bridge.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Bounds-check without offset+length wrap. Offsets never become addresses
 * until the trusted host has verified the full 32-bit range. */
static int cage_range(const atto_fs_trusted_guest_frame *frame,
                      uint32_t off, uint32_t length) {
    if (!frame || !frame->broker ||
        frame->broker->policy_layer_count != ATTO_FS_POLICY_LAYER_COUNT ||
        !frame->cage || !frame->cage_size ||
        !frame->tenant_id || !frame->generation) return -EACCES;
    /* The VM's cage is bigger than any one actor's private region. Bounds
     * against cage_size alone would permit an actor to read/write another
     * actor's state via its host-I/O paths and output offset. */
    const size_t start = (size_t)frame->actor_region_start;
    const size_t region_bytes = (size_t)frame->actor_region_bytes;
    if (start == 0 || region_bytes == 0 || frame->cage_size > UINT32_MAX ||
        start > frame->cage_size ||
        region_bytes > frame->cage_size - start) return -EACCES;
    if ((size_t)off < start || (size_t)off - start > region_bytes ||
        (size_t)length > region_bytes - ((size_t)off - start)) return -EFAULT;
    return 0;
}
static int load_path(const atto_fs_trusted_guest_frame *frame,
                     uint32_t off, uint32_t len,
                     char path[ATTO_FS_SCOPE_LIMIT]) {
    if (!len || len >= ATTO_FS_SCOPE_LIMIT) return -EINVAL;
    int result = cage_range(frame, off, len);
    if (result) return result;
    const uint8_t *raw = frame->cage + (size_t)off;
    if (memchr(raw, 0, (size_t)len) != NULL) return -EINVAL;
    /* v1 deliberately admits only portable ASCII relative paths. The
     * complete UTF-8 and normalization contract requires a later version. */
    for (uint32_t i = 0; i < len; ++i) {
        if (raw[i] >= 128u || raw[i] < 32u || raw[i] == 127u) return -EINVAL;
    }
    memcpy(path, raw, (size_t)len);
    path[len] = '\0';
    return 0;
}
int atto_fs_guest_read(const atto_fs_trusted_guest_frame *frame,
                       uint32_t path_off, uint32_t path_len,
                       uint32_t out_off, uint32_t out_cap, uint32_t *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!bytes_read || out_cap > ATTO_FS_MAX_BYTES) return -EINVAL;
    char path[ATTO_FS_SCOPE_LIMIT];
    int rc = load_path(frame, path_off, path_len, path);
    if (rc) return rc;
    rc = cage_range(frame, out_off, out_cap);
    if (rc) return rc;
    /* The broker reads into a private staging buffer and copies into the
     * caller output ONLY on success, while holding the same mutex as revoke.
     * Calling it with this already-validated actor output span keeps the
     * entire read->guest-copy commit atomic with respect to revocation.
     * Copying our own staging buffer AFTER broker_read() returned would
     * permit revoked I/O to write into guest memory after revoke completed. */
    size_t got = 0u;
    rc = atto_fs_broker_read(frame->broker, frame->tenant_id, frame->generation,
                             path, frame->cage + (size_t)out_off, (size_t)out_cap, &got);
    if (rc == 0) {
        if (got > (size_t)out_cap || got > UINT32_MAX) {
            rc = -EOVERFLOW;
        } else {
            *bytes_read = (uint32_t)got;
        }
    }
    return rc;
}
int atto_fs_guest_write(const atto_fs_trusted_guest_frame *frame,
                        uint32_t path_off, uint32_t path_len,
                        uint32_t payload_off, uint32_t payload_len) {
    if (payload_len > ATTO_FS_MAX_BYTES) return -EINVAL;
    char path[ATTO_FS_SCOPE_LIMIT];
    int rc = load_path(frame, path_off, path_len, path);
    if (rc) return rc;
    rc = cage_range(frame, payload_off, payload_len);
    if (rc) return rc;
    /* Snapshot guest-controlled bytes before any host I/O or suspension. */
    uint8_t *snapshot = (uint8_t *)malloc((size_t)payload_len + 1u);
    if (!snapshot) return -ENOMEM;
    memcpy(snapshot, frame->cage + (size_t)payload_off, (size_t)payload_len);
    rc = atto_fs_broker_write(frame->broker, frame->tenant_id, frame->generation,
                              path, snapshot, (size_t)payload_len);
    free(snapshot);
    return rc;
}
