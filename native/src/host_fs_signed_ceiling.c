#include "atto/host_fs_signed_ceiling.h"
#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <string.h>

#define CEILING_WIRE_LIMIT (sizeof(ATTO_FS_CEILING_MAGIC) - 1u + 32u + 1u + 4u * (2u + ATTO_FS_SIGNED_SCOPE_LIMIT - 1u))

static int valid_scope_bytes(const uint8_t *s, size_t len) {
    if (!len) return 1; /* missing grant / no explicit deny */
    if (len == 1u && s[0] == '*') return 1;
    if (s[0] == '/' || s[0] == '-') return 0;
    size_t component = 0u;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || s[i] == '/') {
            const size_t n = i - component;
            if (!n || (n == 1 && s[component] == '.') ||
                (n == 2 && s[component] == '.' && s[component + 1] == '.')) return 0;
            component = i + 1u;
            continue;
        }
        const uint8_t c = s[i];
        if (c < 32u || c > 126u || c == ':' || c == '\\' || c == '*') return 0;
    }
    return 1;
}

static int decode_scopes(const uint8_t *descriptor, size_t size,
                         atto_fs_verified_ceiling *out) {
    const size_t prefix = sizeof(ATTO_FS_CEILING_MAGIC) - 1u;
    if (size < prefix + 33u + 8u || size > CEILING_WIRE_LIMIT ||
        memcmp(descriptor, ATTO_FS_CEILING_MAGIC, prefix) != 0 ||
        descriptor[prefix + 32u] != 0xD1u) return -EINVAL;
    memcpy(out->artifact_sha256, descriptor + prefix, 32u);
    char *dest[4] = {out->allow_read, out->allow_write, out->deny_read, out->deny_write};
    size_t cursor = prefix + 33u;
    for (size_t i = 0; i < 4u; ++i) {
        if (size - cursor < 2u) return -EINVAL;
        const size_t n = (size_t)descriptor[cursor] | ((size_t)descriptor[cursor + 1u] << 8u);
        cursor += 2u;
        if (n >= ATTO_FS_SIGNED_SCOPE_LIMIT || n > size - cursor ||
            !valid_scope_bytes(descriptor + cursor, n)) return -EINVAL;
        memcpy(dest[i], descriptor + cursor, n);
        dest[i][n] = '\0';
        cursor += n;
    }
    return cursor == size ? 0 : -EINVAL;
}

int atto_fs_verify_signed_ceiling_v1(
    const uint8_t *artifact, size_t artifact_size,
    const uint8_t *descriptor, size_t descriptor_size,
    const uint8_t signature[ATTO_FS_ED25519_SIGNATURE_BYTES],
    const uint8_t trusted_public_key[ATTO_FS_ED25519_PUBLIC_KEY_BYTES],
    atto_fs_verified_ceiling *out) {
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!artifact || !artifact_size || artifact_size > ATTO_FS_SIGNED_MAX_ARTIFACT ||
        !descriptor || !descriptor_size || descriptor_size > CEILING_WIRE_LIMIT ||
        !signature || !trusted_public_key) return -EINVAL;
    /* Authentication happens before parsing any variable-length policy. */
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                 trusted_public_key, ATTO_FS_ED25519_PUBLIC_KEY_BYTES);
    if (!key) return -EACCES;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { EVP_PKEY_free(key); return -ENOMEM; }
    int rc = -EACCES;
    if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
        EVP_DigestVerify(ctx, signature, ATTO_FS_ED25519_SIGNATURE_BYTES,
                         descriptor, descriptor_size) == 1) {
        atto_fs_verified_ceiling decoded = {0};
        if (decode_scopes(descriptor, descriptor_size, &decoded) == 0) {
            uint8_t digest[32];
            unsigned int length = 0;
            if (EVP_Digest(artifact, artifact_size, digest, &length, EVP_sha256(), NULL) == 1 &&
                length == sizeof(digest) &&
                CRYPTO_memcmp(decoded.artifact_sha256, digest, sizeof(digest)) == 0) {
                *out = decoded;
                rc = 0;
            }
        }
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return rc;
}
