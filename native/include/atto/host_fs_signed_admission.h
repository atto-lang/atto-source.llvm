#ifndef ATTO_HOST_FS_SIGNED_ADMISSION_H
#define ATTO_HOST_FS_SIGNED_ADMISSION_H
#include "atto/host_fs_broker.h"
#include "atto/host_fs_signed_ceiling.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Host-only admission boundary between signed artifact bytes and the six-
 * ceiling scoped broker. The caller is the TRUSTED LOADER, never guest code.
 * artifact_bytes MUST be the very bytes that are mapped as executable code
 * (a sealed immutable image). Verifying a second pathname is forbidden.
 *
 * The public key must come from an independently protected host trust store;
 * the five other ceilings and tenant identity MUST be trusted scheduler state.
 * This function authenticates ONLY the signed artifact ceiling; it does NOT
 * authenticate the caller-supplied other ceilings or provide an OS sandbox.
 */
typedef struct atto_fs_signed_launch {
    const unsigned char *artifact_bytes;
    size_t artifact_size;
    const unsigned char *signed_descriptor;
    size_t signed_descriptor_size;
    const unsigned char *signature_ed25519; /* exactly 64 bytes */
    const unsigned char *trusted_public_key; /* exactly 32 bytes */
    const char *trusted_root; /* host-provided absolute root path */
    uint64_t tenant_id;
    uint64_t generation;
    atto_fs_policy_layer host;
    atto_fs_policy_layer runtime;
    atto_fs_policy_layer tenant;
    atto_fs_policy_layer actor;
    atto_fs_policy_layer delegation;
} atto_fs_signed_launch;

/*
 * On success broker has all six independent immutable allow/deny layers;
 * its artifact layer is populated SOLELY from the authenticated descriptor.
 * On failure broker remains uninitialized and must not be used.
 * The caller must supply a fresh broker object and ensure its lifetime exceeds
 * every worker; do not reinitialize or close concurrently.
 */
int atto_fs_broker_admit_signed(atto_fs_broker *broker,
                                const atto_fs_signed_launch *launch);
#ifdef __cplusplus
}
#endif
#endif
