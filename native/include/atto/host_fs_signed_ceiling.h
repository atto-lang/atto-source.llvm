#ifndef ATTO_HOST_FS_SIGNED_CEILING_H
#define ATTO_HOST_FS_SIGNED_CEILING_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host-only signed ARTIFACT maximum authority, not a runtime grant.
 * The trusted key is supplied by a protected host trust store, never by a
 * guest, unsigned artifact, or the descriptor itself. The signing tool is
 * deliberately NOT included in the production library. */
#define ATTO_FS_SIGNED_MAX_ARTIFACT (64u * 1024u * 1024u)
#define ATTO_FS_SIGNED_SCOPE_LIMIT 1024u
#define ATTO_FS_ED25519_PUBLIC_KEY_BYTES 32u
#define ATTO_FS_ED25519_SIGNATURE_BYTES 64u
#define ATTO_FS_CEILING_MAGIC "ATTO-FS-CEILING-V1"
/* Wire: ASCII magic (without NUL), SHA-256(artifact bytes), literal 0xD1
 * (all explicit deny fields present), then four ordered (u16 LE byte length,
 * canonical ASCII bytes) fields: allow_read, allow_write, deny_read,
 * deny_write. Empty scope = deny all on allow or no explicit deny. '*' means
 * category-wide scope. No NUL terminator or trailing bytes are signed. */
typedef struct atto_fs_verified_ceiling {
    uint8_t artifact_sha256[32];
    char allow_read[ATTO_FS_SIGNED_SCOPE_LIMIT];
    char allow_write[ATTO_FS_SIGNED_SCOPE_LIMIT];
    char deny_read[ATTO_FS_SIGNED_SCOPE_LIMIT];
    char deny_write[ATTO_FS_SIGNED_SCOPE_LIMIT];
} atto_fs_verified_ceiling;

/* Zero out on failure; return 0 only after Ed25519 signature, exact artifact
 * hash, mandatory explicit-deny marker and canonical scope validation pass.
 * WARNING: the output is plain mutable host memory, NOT a bearer credential;
 * only a trusted loader may transfer it to a broker constructor. */
int atto_fs_verify_signed_ceiling_v1(
    const uint8_t *artifact, size_t artifact_size,
    const uint8_t *descriptor, size_t descriptor_size,
    const uint8_t signature[ATTO_FS_ED25519_SIGNATURE_BYTES],
    const uint8_t trusted_public_key[ATTO_FS_ED25519_PUBLIC_KEY_BYTES],
    atto_fs_verified_ceiling *out);
#ifdef __cplusplus
}
#endif
#endif
