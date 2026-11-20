#ifndef ATTO_HOST_FS_GUEST_BRIDGE_H
#define ATTO_HOST_FS_GUEST_BRIDGE_H
#include "atto/host_fs_broker.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host-side candidate adapter for a future std.fs guest lowering.
 * This is NOT a public or stable compiler ABI, and not a TenantVM sandbox.
 * The frame is constructed and retained ONLY by the trusted host runtime.
 * The guest supplies 32-bit offsets/lengths, NEVER host pointers/IDs/FDs.
 * Caller must not run guest code concurrently with a pending cage copy.
 */
typedef struct atto_fs_trusted_guest_frame {
    atto_fs_broker *broker;        /* trusted host allocation, never guest-visible */
    uint64_t tenant_id;           /* authenticated by host scheduler, NOT from guest */
    uint64_t generation;          /* authenticated by host scheduler, NOT from guest */
    uint8_t *cage;                /* verified private guest-memory window */
    size_t cage_size;             /* exact accessible extent supplied by host */
} atto_fs_trusted_guest_frame;

/* All lengths are bytes. On denial no output bytes or filesystem bytes change.
 * Each operation returns 0 or a negative errno and must be invoked from a
 * bounded host worker; these calls are synchronous and may block its thread.
 * On read, bytes_read is always reset to 0 on failure. */
int atto_fs_guest_read(const atto_fs_trusted_guest_frame *host_frame,
                       uint32_t guest_path_offset, uint32_t guest_path_length,
                       uint32_t guest_out_offset, uint32_t guest_out_capacity,
                       uint32_t *bytes_read);
int atto_fs_guest_write(const atto_fs_trusted_guest_frame *host_frame,
                        uint32_t guest_path_offset, uint32_t guest_path_length,
                        uint32_t guest_payload_offset, uint32_t guest_payload_length);
#ifdef __cplusplus
}
#endif
#endif
